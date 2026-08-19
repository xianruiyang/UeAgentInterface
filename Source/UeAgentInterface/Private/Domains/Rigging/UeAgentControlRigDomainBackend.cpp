// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Rigging/UeAgentControlRigDomainBackend.h"

#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"
#include "Utilities/UeAgentAuthoringFileIO.h"
#include "Utilities/UeAgentAuthoringJson.h"
#include "Utilities/UeAgentDetailsAuthoring.h"
#include "Utilities/UeAgentFolderDetailsExport.h"
#include "Utilities/UeAgentJsonDiagnostics.h"
#include "Utilities/UeAgentStructuredAuthoringIO.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimSequence.h"
#include "ControlRig.h"
#include "ControlRigBlueprint.h"
#include "ControlRigBlueprintFactory.h"
#include "ControlRigGizmoLibrary.h"
#include "ControlRigSequencerEditorLibrary.h"
#include "Components/SkeletalMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "Animation/Skeleton.h"
#include "Exporters/AnimSeqExportOption.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "LevelSequence.h"
#include "LevelSequenceActor.h"
#include "LevelSequencePlayer.h"
#include "Materials/Material.h"
#include "MovieScene.h"
#include "MovieSceneBindingProxy.h"
#include "MovieSceneSequenceID.h"
#include "MovieSceneSequencePlayer.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "RigVMModel/Nodes/RigVMTemplateNode.h"
#include "RigVMModel/Nodes/RigVMUnitNode.h"
#include "RigVMModel/Nodes/RigVMVariableNode.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMGraph.h"
#include "RigVMModel/RigVMLink.h"
#include "RigVMModel/RigVMNode.h"
#include "RigVMModel/RigVMPin.h"
#include "Rigs/FKControlRig.h"
#include "Rigs/RigHierarchy.h"
#include "Rigs/RigHierarchyController.h"
#include "SequencerTools.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Units/Execution/RigUnit_BeginExecution.h"
#include "Units/Execution/RigUnit_InverseExecution.h"
#include "Units/Execution/RigUnit_PrepareForExecution.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

namespace UeAgentControlRigOps
{
	FString NormalizeAssetPath(const FString& InPath)
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
		return LoadObject<UObject>(nullptr, *ObjectPath);
	}

	template <typename T>
	static T* LoadAsset(const FString& InPath)
	{
		return Cast<T>(LoadAssetObject(InPath));
	}

	UControlRigBlueprint* LoadControlRigBlueprint(const FString& InPath)
	{
		return LoadAsset<UControlRigBlueprint>(InPath);
	}

	UControlRigShapeLibrary* LoadShapeLibrary(const FString& InPath)
	{
		return LoadAsset<UControlRigShapeLibrary>(InPath);
	}

	static UScriptStruct* ResolveStructByPathOrAlias(const FString& InTypeName)
	{
		FString TypeName = InTypeName.TrimStartAndEnd();
		if (TypeName.IsEmpty())
		{
			return nullptr;
		}
		if (UScriptStruct* DirectStruct = LoadObject<UScriptStruct>(nullptr, *TypeName))
		{
			return DirectStruct;
		}

		FString Token = TypeName.ToLower();
		Token.RemoveFromStart(TEXT("f"));
		Token.ReplaceInline(TEXT("_"), TEXT(""));
		Token.ReplaceInline(TEXT(" "), TEXT(""));
		struct FAlias
		{
			const TCHAR* Alias;
			const TCHAR* Path;
		};
		static const FAlias Aliases[] = {
			{ TEXT("vector"), TEXT("/Script/CoreUObject.Vector") },
			{ TEXT("vector2d"), TEXT("/Script/CoreUObject.Vector2D") },
			{ TEXT("rotator"), TEXT("/Script/CoreUObject.Rotator") },
			{ TEXT("quat"), TEXT("/Script/CoreUObject.Quat") },
			{ TEXT("transform"), TEXT("/Script/CoreUObject.Transform") },
			{ TEXT("linearcolor"), TEXT("/Script/CoreUObject.LinearColor") },
			{ TEXT("color"), TEXT("/Script/CoreUObject.LinearColor") },
			{ TEXT("rigelementkey"), TEXT("/Script/ControlRig.RigElementKey") },
			{ TEXT("eulertransform"), TEXT("/Script/ControlRig.EulerTransform") }
		};
		for (const FAlias& Alias : Aliases)
		{
			if (Token == Alias.Alias)
			{
				return LoadObject<UScriptStruct>(nullptr, Alias.Path);
			}
		}
		return nullptr;
	}

	static UClass* ResolveClassByPathOrAlias(const FString& InTypeName)
	{
		FString TypeName = InTypeName.TrimStartAndEnd();
		if (TypeName.IsEmpty())
		{
			return nullptr;
		}
		if (UClass* DirectClass = LoadObject<UClass>(nullptr, *TypeName))
		{
			return DirectClass;
		}

		FString Token = TypeName.ToLower();
		Token.RemoveFromStart(TEXT("u"));
		Token.RemoveFromStart(TEXT("a"));
		Token.ReplaceInline(TEXT("_"), TEXT(""));
		Token.ReplaceInline(TEXT(" "), TEXT(""));
		struct FAlias
		{
			const TCHAR* Alias;
			const TCHAR* Path;
		};
		static const FAlias Aliases[] = {
			{ TEXT("object"), TEXT("/Script/CoreUObject.Object") },
			{ TEXT("actor"), TEXT("/Script/Engine.Actor") },
			{ TEXT("skeletalmesh"), TEXT("/Script/Engine.SkeletalMesh") },
			{ TEXT("skeleton"), TEXT("/Script/Engine.Skeleton") },
			{ TEXT("controlrig"), TEXT("/Script/ControlRig.ControlRig") },
			{ TEXT("controlrigblueprint"), TEXT("/Script/ControlRigDeveloper.ControlRigBlueprint") },
			{ TEXT("animsequence"), TEXT("/Script/Engine.AnimSequence") },
			{ TEXT("levelsequence"), TEXT("/Script/LevelSequence.LevelSequence") }
		};
		for (const FAlias& Alias : Aliases)
		{
			if (Token == Alias.Alias)
			{
				return LoadObject<UClass>(nullptr, Alias.Path);
			}
		}
		return nullptr;
	}

	static FString PinContainerTypeToString(const EPinContainerType ContainerType)
	{
		switch (ContainerType)
		{
		case EPinContainerType::Array: return TEXT("array");
		case EPinContainerType::Set: return TEXT("set");
		case EPinContainerType::Map: return TEXT("map");
		default: return FString();
		}
	}

	static bool ParsePinContainerType(const FString& InText, EPinContainerType& OutType)
	{
		const FString Text = InText.TrimStartAndEnd().ToLower();
		if (Text.IsEmpty() || Text == TEXT("none"))
		{
			OutType = EPinContainerType::None;
			return true;
		}
		if (Text == TEXT("array"))
		{
			OutType = EPinContainerType::Array;
			return true;
		}
		if (Text == TEXT("set"))
		{
			OutType = EPinContainerType::Set;
			return true;
		}
		if (Text == TEXT("map"))
		{
			OutType = EPinContainerType::Map;
			return true;
		}
		return false;
	}

	static FString PinTypeToCppType(const FEdGraphPinType& PinType)
	{
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean) return TEXT("bool");
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_Int) return TEXT("int32");
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_Int64) return TEXT("int64");
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_Name) return TEXT("FName");
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_String) return TEXT("FString");
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_Text) return TEXT("FText");
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_Real)
		{
			return PinType.PinSubCategory == UEdGraphSchema_K2::PC_Double ? TEXT("double") : TEXT("float");
		}
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
		{
			if (const UScriptStruct* Struct = Cast<UScriptStruct>(PinType.PinSubCategoryObject.Get()))
			{
				return Struct->GetStructCPPName();
			}
		}
		return PinType.PinCategory.ToString();
	}

	static TSharedPtr<FJsonObject> MakePinTypeJson(const FEdGraphPinType& PinType)
	{
		TSharedPtr<FJsonObject> TypeObj = MakeShared<FJsonObject>();
		TypeObj->SetStringField(TEXT("pin_category"), PinType.PinCategory.ToString());
		if (!PinType.PinSubCategory.IsNone())
		{
			TypeObj->SetStringField(TEXT("pin_subcategory"), PinType.PinSubCategory.ToString());
		}
		if (const UObject* SubCategoryObj = PinType.PinSubCategoryObject.Get())
		{
			TypeObj->SetStringField(TEXT("pin_subcategory_object"), SubCategoryObj->GetPathName());
		}
		const FString ContainerText = PinContainerTypeToString(PinType.ContainerType);
		if (!ContainerText.IsEmpty())
		{
			TypeObj->SetStringField(TEXT("container_type"), ContainerText);
		}
		if (PinType.ContainerType == EPinContainerType::Map && !PinType.PinValueType.TerminalCategory.IsNone())
		{
			TSharedPtr<FJsonObject> ValueTypeObj = MakeShared<FJsonObject>();
			ValueTypeObj->SetStringField(TEXT("pin_category"), PinType.PinValueType.TerminalCategory.ToString());
			if (!PinType.PinValueType.TerminalSubCategory.IsNone())
			{
				ValueTypeObj->SetStringField(TEXT("pin_subcategory"), PinType.PinValueType.TerminalSubCategory.ToString());
			}
			if (const UObject* ValueSubCategoryObj = PinType.PinValueType.TerminalSubCategoryObject.Get())
			{
				ValueTypeObj->SetStringField(TEXT("pin_subcategory_object"), ValueSubCategoryObj->GetPathName());
			}
			TypeObj->SetObjectField(TEXT("value_type"), ValueTypeObj);
		}
		return TypeObj;
	}

	static TSharedPtr<FJsonObject> BuildVariableJson(UControlRigBlueprint* Blueprint, const FBPVariableDescription& VarDesc)
	{
		TSharedPtr<FJsonObject> VarObj = MakeShared<FJsonObject>();
		VarObj->SetStringField(TEXT("name"), VarDesc.VarName.ToString());
		VarObj->SetObjectField(TEXT("type"), MakePinTypeJson(VarDesc.VarType));
		VarObj->SetStringField(TEXT("cpp_type"), PinTypeToCppType(VarDesc.VarType));
		if (Blueprint && Blueprint->GeneratedClass)
		{
			if (UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject(false))
			{
				if (FProperty* Property = FindFProperty<FProperty>(Blueprint->GeneratedClass, VarDesc.VarName))
				{
					UeAgentAuthoringState::FStateEntry Entry;
					TSharedPtr<FJsonObject> Descriptor;
					FString CaptureError;
					if (UeAgentDetailsAuthoringService::CaptureObjectProperty(
							CDO, Property->GetName(), UeAgentDetailsAuthoringService::FCapturePolicy(),
							Entry, Descriptor, CaptureError))
					{
						VarObj->SetObjectField(TEXT("default_property"), Descriptor);
					}
				}
			}
		}
		VarObj->SetBoolField(TEXT("instance_editable"), (VarDesc.PropertyFlags & CPF_DisableEditOnInstance) == 0);
		VarObj->SetBoolField(TEXT("read_only"), (VarDesc.PropertyFlags & CPF_BlueprintReadOnly) != 0);
		return VarObj;
	}

	TSharedPtr<FJsonObject> BuildVariablesJson(UControlRigBlueprint* Blueprint)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.control_rig.variables.v1"));
		TArray<TSharedPtr<FJsonValue>> Variables;
		TArray<FBPVariableDescription> SortedVariables = Blueprint ? Blueprint->NewVariables : TArray<FBPVariableDescription>();
		SortedVariables.Sort([](const FBPVariableDescription& A, const FBPVariableDescription& B)
		{
			return A.VarName.LexicalLess(B.VarName);
		});
		for (const FBPVariableDescription& VarDesc : SortedVariables)
		{
			if (VarDesc.VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate)
			{
				continue;
			}
			Variables.Add(MakeShared<FJsonValueObject>(BuildVariableJson(Blueprint, VarDesc)));
		}
		Root->SetArrayField(TEXT("variables"), Variables);
		Root->SetNumberField(TEXT("variable_count"), Variables.Num());
		Root->SetBoolField(TEXT("delete_missing"), true);
		return Root;
	}

	static bool BuildPinTypeFromJsonObject(const TSharedPtr<FJsonObject>& Obj, FEdGraphPinType& OutPinType, FString& OutError)
	{
		if (!Obj.IsValid())
		{
			OutError = TEXT("missing_pin_type");
			return false;
		}
		FString Category;
		if (!Obj->TryGetStringField(TEXT("pin_category"), Category) || Category.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("missing_pin_category");
			return false;
		}

		FString CategoryToken = Category.TrimStartAndEnd().ToLower();
		OutPinType = FEdGraphPinType();
		if (CategoryToken == TEXT("bool") || CategoryToken == TEXT("boolean"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
		}
		else if (CategoryToken == TEXT("int") || CategoryToken == TEXT("int32"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int;
		}
		else if (CategoryToken == TEXT("int64"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int64;
		}
		else if (CategoryToken == TEXT("float") || CategoryToken == TEXT("double") || CategoryToken == TEXT("real"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Real;
			OutPinType.PinSubCategory = CategoryToken == TEXT("double") ? UEdGraphSchema_K2::PC_Double : UEdGraphSchema_K2::PC_Float;
		}
		else if (CategoryToken == TEXT("name"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Name;
		}
		else if (CategoryToken == TEXT("string"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_String;
		}
		else if (CategoryToken == TEXT("text"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Text;
		}
		else if (CategoryToken == TEXT("byte"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
		}
		else if (CategoryToken == TEXT("object") || CategoryToken == TEXT("class") || CategoryToken == TEXT("softobject") || CategoryToken == TEXT("softclass"))
		{
			OutPinType.PinCategory = CategoryToken == TEXT("class") ? UEdGraphSchema_K2::PC_Class :
				CategoryToken == TEXT("softobject") ? UEdGraphSchema_K2::PC_SoftObject :
				CategoryToken == TEXT("softclass") ? UEdGraphSchema_K2::PC_SoftClass :
				UEdGraphSchema_K2::PC_Object;
		}
		else
		{
			OutPinType.PinCategory = FName(*Category.TrimStartAndEnd());
		}

		FString SubCategory;
		Obj->TryGetStringField(TEXT("pin_subcategory"), SubCategory);
		if (!SubCategory.TrimStartAndEnd().IsEmpty())
		{
			if (OutPinType.PinCategory == UEdGraphSchema_K2::PC_Real)
			{
				OutPinType.PinSubCategory = SubCategory.Equals(TEXT("double"), ESearchCase::IgnoreCase) ? UEdGraphSchema_K2::PC_Double : UEdGraphSchema_K2::PC_Float;
			}
			else
			{
				OutPinType.PinSubCategory = FName(*SubCategory.TrimStartAndEnd());
			}
		}

		FString SubCategoryObjectPath;
		Obj->TryGetStringField(TEXT("pin_subcategory_object"), SubCategoryObjectPath);
		if (!SubCategoryObjectPath.TrimStartAndEnd().IsEmpty())
		{
			if (OutPinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
			{
				OutPinType.PinSubCategoryObject = ResolveStructByPathOrAlias(SubCategoryObjectPath);
			}
			else
			{
				UObject* LoadedObject = LoadAssetObject(SubCategoryObjectPath);
				OutPinType.PinSubCategoryObject = LoadedObject ? LoadedObject : ResolveClassByPathOrAlias(SubCategoryObjectPath);
			}
			if (!OutPinType.PinSubCategoryObject.IsValid())
			{
				OutError = TEXT("invalid_pin_subcategory_object");
				return false;
			}
		}

		if (UScriptStruct* Struct = ResolveStructByPathOrAlias(Category))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			OutPinType.PinSubCategory = NAME_None;
			OutPinType.PinSubCategoryObject = Struct;
		}
		if (OutPinType.PinCategory == UEdGraphSchema_K2::PC_Struct && !OutPinType.PinSubCategoryObject.IsValid())
		{
			if (UScriptStruct* Struct = ResolveStructByPathOrAlias(SubCategory))
			{
				OutPinType.PinSubCategoryObject = Struct;
			}
		}
		if (OutPinType.PinCategory == UEdGraphSchema_K2::PC_Struct && !OutPinType.PinSubCategoryObject.IsValid())
		{
			OutError = TEXT("missing_struct_pin_subcategory_object");
			return false;
		}
		if ((OutPinType.PinCategory == UEdGraphSchema_K2::PC_Object
			|| OutPinType.PinCategory == UEdGraphSchema_K2::PC_Class
			|| OutPinType.PinCategory == UEdGraphSchema_K2::PC_SoftObject
			|| OutPinType.PinCategory == UEdGraphSchema_K2::PC_SoftClass)
			&& !OutPinType.PinSubCategoryObject.IsValid())
		{
			OutPinType.PinSubCategoryObject = UObject::StaticClass();
		}

		FString ContainerTypeText;
		Obj->TryGetStringField(TEXT("container_type"), ContainerTypeText);
		if (!ParsePinContainerType(ContainerTypeText, OutPinType.ContainerType))
		{
			OutError = TEXT("invalid_container_type");
			return false;
		}
		if (OutPinType.ContainerType == EPinContainerType::Map)
		{
			const TSharedPtr<FJsonObject>* ValueTypeObj = nullptr;
			if (!Obj->TryGetObjectField(TEXT("value_type"), ValueTypeObj) || !ValueTypeObj || !ValueTypeObj->IsValid())
			{
				OutError = TEXT("missing_map_value_type");
				return false;
			}
			FEdGraphPinType ValuePinType;
			if (!BuildPinTypeFromJsonObject(*ValueTypeObj, ValuePinType, OutError))
			{
				return false;
			}
			OutPinType.PinValueType.TerminalCategory = ValuePinType.PinCategory;
			OutPinType.PinValueType.TerminalSubCategory = ValuePinType.PinSubCategory;
			OutPinType.PinValueType.TerminalSubCategoryObject = ValuePinType.PinSubCategoryObject;
		}
		return true;
	}

	static bool BuildPinTypeFromVariableJson(const TSharedPtr<FJsonObject>& VarObj, FEdGraphPinType& OutPinType, FString& OutError)
	{
		const TSharedPtr<FJsonObject>* TypeObj = nullptr;
		if (VarObj.IsValid() && VarObj->TryGetObjectField(TEXT("type"), TypeObj) && TypeObj && TypeObj->IsValid())
		{
			return BuildPinTypeFromJsonObject(*TypeObj, OutPinType, OutError);
		}
		FString CppType;
		if (!VarObj.IsValid() || !VarObj->TryGetStringField(TEXT("cpp_type"), CppType) || CppType.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("missing_variable_type");
			return false;
		}
		TSharedPtr<FJsonObject> TypeFromCpp = MakeShared<FJsonObject>();
		const FString TypeToken = CppType.TrimStartAndEnd().ToLower();
		if (TypeToken == TEXT("float") || TypeToken == TEXT("double"))
		{
			TypeFromCpp->SetStringField(TEXT("pin_category"), TEXT("real"));
			TypeFromCpp->SetStringField(TEXT("pin_subcategory"), TypeToken);
		}
		else if (TypeToken == TEXT("bool") || TypeToken == TEXT("boolean"))
		{
			TypeFromCpp->SetStringField(TEXT("pin_category"), TEXT("bool"));
		}
		else if (TypeToken == TEXT("int") || TypeToken == TEXT("int32") || TypeToken == TEXT("int64") || TypeToken == TEXT("name") || TypeToken == TEXT("string") || TypeToken == TEXT("text"))
		{
			TypeFromCpp->SetStringField(TEXT("pin_category"), TypeToken);
		}
		else if (UScriptStruct* Struct = ResolveStructByPathOrAlias(CppType))
		{
			TypeFromCpp->SetStringField(TEXT("pin_category"), TEXT("struct"));
			TypeFromCpp->SetStringField(TEXT("pin_subcategory_object"), Struct->GetPathName());
		}
		else if (UClass* Class = ResolveClassByPathOrAlias(CppType))
		{
			TypeFromCpp->SetStringField(TEXT("pin_category"), TEXT("object"));
			TypeFromCpp->SetStringField(TEXT("pin_subcategory_object"), Class->GetPathName());
		}
		else
		{
			OutError = TEXT("unsupported_variable_cpp_type");
			return false;
		}
		return BuildPinTypeFromJsonObject(TypeFromCpp, OutPinType, OutError);
	}

	FControlRigVariableApplyStats::FControlRigVariableApplyStats()
		: Added(0)
		, DefaultsUpdated(0)
		, FlagsUpdated(0)
		, Existing(0)
	{
	}

	void ApplyVariablesJson(UControlRigBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Obj, const FString& Path, TArray<TSharedPtr<FJsonValue>>& Issues, FControlRigVariableApplyStats& Stats)
	{
		if (!Blueprint || !Obj.IsValid())
		{
			return;
		}
		UeAgentJsonDiagnostics::WarnUnknownFields(Obj, Path, { TEXT("schema"), TEXT("variables"), TEXT("variable_count"), TEXT("delete_missing") }, Issues);
		const TArray<TSharedPtr<FJsonValue>>* Variables = nullptr;
		if (!UeAgentJsonDiagnostics::ReadArrayField(Obj, TEXT("variables"), Path + TEXT(".variables"), Variables, Issues, false) || !Variables)
		{
			return;
		}
		struct FPendingVariableDefault
		{
			FGuid VariableGuid;
			FString VariableName;
			TSharedPtr<FJsonObject> Descriptor;
		};
		TArray<FPendingVariableDefault> PendingDefaults;
		TSet<FName> DesiredVariables;

		for (int32 VariableIndex = 0; VariableIndex < Variables->Num(); ++VariableIndex)
		{
			TSharedPtr<FJsonObject> VarObj;
			const FString VarPath = FString::Printf(TEXT("%s.variables[%d]"), *Path, VariableIndex);
			if (!UeAgentJsonDiagnostics::ReadObjectFromValue((*Variables)[VariableIndex], VarPath, VarObj, Issues))
			{
				continue;
			}
			UeAgentJsonDiagnostics::WarnUnknownFields(VarObj, VarPath, { TEXT("name"), TEXT("cpp_type"), TEXT("type"), TEXT("default_property"), TEXT("direction"), TEXT("expose_to_anim_instance"), TEXT("instance_editable"), TEXT("read_only"), TEXT("category"), TEXT("metadata") }, Issues);

			FString Name;
			if (!UeAgentJsonDiagnostics::ReadStringField(VarObj, TEXT("name"), VarPath + TEXT(".name"), Name, Issues, true) || Name.TrimStartAndEnd().IsEmpty())
			{
				continue;
			}
			FEdGraphPinType PinType;
			FString TypeError;
			if (!BuildPinTypeFromVariableJson(VarObj, PinType, TypeError))
			{
				UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TypeError, VarPath + TEXT(".type"), FString::Printf(TEXT("Invalid Control Rig variable type for '%s'."), *Name));
				continue;
			}

			const TSharedPtr<FJsonObject>* DefaultProperty = nullptr;
			VarObj->TryGetObjectField(TEXT("default_property"), DefaultProperty);

			const FName VariableName(*Name);
			DesiredVariables.Add(VariableName);
			const int32 ExistingIndex = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VariableName);
			if (ExistingIndex == INDEX_NONE)
			{
				if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, VariableName, PinType, FString()))
				{
					UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("control_rig_add_variable_failed"), VarPath, FString::Printf(TEXT("Failed to add variable '%s'."), *Name));
					continue;
				}
				++Stats.Added;
			}
			else
			{
				const FBPVariableDescription& ExistingVar = Blueprint->NewVariables[ExistingIndex];
				const bool bSameType =
					ExistingVar.VarType.PinCategory == PinType.PinCategory
					&& ExistingVar.VarType.PinSubCategory == PinType.PinSubCategory
					&& ExistingVar.VarType.ContainerType == PinType.ContainerType
					&& ExistingVar.VarType.PinSubCategoryObject.Get() == PinType.PinSubCategoryObject.Get();
				if (!bSameType)
				{
					UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("control_rig_variable_type_update_not_supported"), VarPath + TEXT(".type"), FString::Printf(TEXT("Variable '%s' already exists with another type. Rename/remove explicitly before changing type."), *Name));
					continue;
				}
				++Stats.Existing;
			}

			if (DefaultProperty && DefaultProperty->IsValid())
			{
				const int32 ResolvedIndex = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VariableName);
				if (ResolvedIndex == INDEX_NONE || !Blueprint->NewVariables[ResolvedIndex].VarGuid.IsValid())
				{
					UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("control_rig_variable_guid_unavailable"), VarPath, FString::Printf(TEXT("Variable '%s' has no UE variable GUID."), *Name));
					continue;
				}
				PendingDefaults.Add({ Blueprint->NewVariables[ResolvedIndex].VarGuid, Name, *DefaultProperty });
			}

			bool bInstanceEditable = true;
			if (UeAgentJsonDiagnostics::ReadBoolField(VarObj, TEXT("instance_editable"), VarPath + TEXT(".instance_editable"), bInstanceEditable, Issues, false))
			{
				FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(Blueprint, VariableName, !bInstanceEditable);
				++Stats.FlagsUpdated;
			}

			bool bReadOnly = false;
			if (UeAgentJsonDiagnostics::ReadBoolField(VarObj, TEXT("read_only"), VarPath + TEXT(".read_only"), bReadOnly, Issues, false))
			{
				if (uint64* PropertyFlags = FBlueprintEditorUtils::GetBlueprintVariablePropertyFlags(Blueprint, VariableName))
				{
					if (bReadOnly)
					{
						*PropertyFlags |= CPF_BlueprintReadOnly;
					}
					else
					{
						*PropertyFlags &= ~CPF_BlueprintReadOnly;
					}
					++Stats.FlagsUpdated;
				}
			}

			FString Category;
			if (VarObj->TryGetStringField(TEXT("category"), Category) && !Category.TrimStartAndEnd().IsEmpty())
			{
				FBlueprintEditorUtils::SetBlueprintVariableCategory(Blueprint, VariableName, nullptr, FText::FromString(Category), true);
			}

			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		}

		bool bDeleteMissing = false;
		Obj->TryGetBoolField(TEXT("delete_missing"), bDeleteMissing);
		if (bDeleteMissing && !HasBlockingIssue(Issues))
		{
			TArray<FName> VariablesToRemove;
			for (const FBPVariableDescription& Existing : Blueprint->NewVariables)
			{
				if (Existing.VarType.PinCategory != UEdGraphSchema_K2::PC_MCDelegate
					&& !DesiredVariables.Contains(Existing.VarName))
				{
					VariablesToRemove.Add(Existing.VarName);
				}
			}
			for (const FName VariableName : VariablesToRemove)
			{
				FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, VariableName);
			}
			if (!VariablesToRemove.IsEmpty())
			{
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
			}
		}

		if (!PendingDefaults.IsEmpty())
		{
			FCompilerResultsLog LocalCompileLog;
			FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &LocalCompileLog);
			UObject* CDO = Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetDefaultObject(false) : nullptr;
			if (!CDO)
			{
				UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("control_rig_variable_default_cdo_unavailable"), Path, TEXT("Generated Control Rig CDO is unavailable."));
				return;
			}
			TArray<TSharedPtr<FJsonValue>> ValidatedDefaults;
			bool bDefaultMappingInvalid = false;
			for (const FPendingVariableDefault& PendingDefault : PendingDefaults)
			{
				const FName GeneratedPropertyName = UBlueprint::GetFieldNameFromClassByGuid<FProperty>(
					Blueprint->GeneratedClass,
					PendingDefault.VariableGuid);
				FProperty* GeneratedProperty = GeneratedPropertyName.IsNone()
					? nullptr
					: FindFProperty<FProperty>(Blueprint->GeneratedClass, GeneratedPropertyName);
				FString ExpectedPropertyPath;
				if (GeneratedProperty)
				{
					ExpectedPropertyPath = GeneratedProperty->GetName();
				}
				FString DefaultPropertyPath;
				if (!GeneratedProperty || ExpectedPropertyPath.IsEmpty()
					|| !PendingDefault.Descriptor->TryGetStringField(TEXT("property_path"), DefaultPropertyPath)
					|| DefaultPropertyPath != ExpectedPropertyPath)
				{
					UeAgentJsonDiagnostics::AddIssue(
						Issues,
						TEXT("error"),
						TEXT("variable_default_property_path_mismatch"),
						Path + TEXT(".variables"),
						FString::Printf(
							TEXT("Variable '%s' default_property must target its UE-generated property '%s'."),
							*PendingDefault.VariableName,
							*ExpectedPropertyPath));
					bDefaultMappingInvalid = true;
					continue;
				}
				ValidatedDefaults.Add(MakeShared<FJsonValueObject>(PendingDefault.Descriptor));
			}
			if (bDefaultMappingInvalid)
			{
				return;
			}
			TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
			Payload->SetStringField(TEXT("schema"), TEXT("uai.uobject_properties.v2"));
			Payload->SetArrayField(TEXT("properties"), ValidatedDefaults);
			UeAgentAuthoringObjectApply::FOptions ApplyOptions;
			ApplyOptions.bAllowTemplateOwners = true;
			ApplyOptions.bAllowDomainAuthorizedOwners = true;
			UeAgentAuthoringDirectApply::FResult ApplyResult;
			FString ApplyError;
			if (!UeAgentDetailsAuthoringService::ApplyObjectProperties(
					CDO, Payload, ApplyOptions, ApplyResult, ApplyError))
			{
				UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("control_rig_variable_default_apply_failed"), Path, TEXT("Canonical variable default apply failed."));
				return;
			}
			Stats.DefaultsUpdated += ApplyResult.PropertiesSeen;
		}
	}

	bool SaveAssetPackage(UObject* Asset, FString& OutError)
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

	bool HasBlockingIssue(const TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		for (const TSharedPtr<FJsonValue>& IssueValue : Issues)
		{
			const TSharedPtr<FJsonObject>* IssueObj = nullptr;
			if (!IssueValue.IsValid() || !IssueValue->TryGetObject(IssueObj) || !IssueObj || !IssueObj->IsValid())
			{
				continue;
			}

			FString Severity;
			FString Code;
			(*IssueObj)->TryGetStringField(TEXT("severity"), Severity);
			(*IssueObj)->TryGetStringField(TEXT("code"), Code);
			if (Severity.Equals(TEXT("error"), ESearchCase::IgnoreCase))
			{
				return true;
			}
			if (!Code.Equals(TEXT("json_unknown_field"), ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	int32 CountIssuesBySeverity(const TArray<TSharedPtr<FJsonValue>>& Issues, const FString& SeverityToMatch)
	{
		int32 Count = 0;
		for (const TSharedPtr<FJsonValue>& IssueValue : Issues)
		{
			const TSharedPtr<FJsonObject>* IssueObj = nullptr;
			if (!IssueValue.IsValid() || !IssueValue->TryGetObject(IssueObj) || !IssueObj || !IssueObj->IsValid())
			{
				continue;
			}
			FString Severity;
			(*IssueObj)->TryGetStringField(TEXT("severity"), Severity);
			if (Severity.Equals(SeverityToMatch, ESearchCase::IgnoreCase))
			{
				++Count;
			}
		}
		return Count;
	}

	static bool MakeJsonString(const TSharedPtr<FJsonObject>& Obj, FString& OutJson)
	{
		return UeAgentJsonDiagnostics::SerializeJsonObjectCompact(Obj, OutJson);
	}

	bool SaveAuthoringFile(const FString& FilePath, const TSharedPtr<FJsonObject>& Obj, FString& OutError)
	{
		UeAgentAuthoringFileIO::FSaveOptions Options;
		Options.SourceCommand = TEXT("control_rig_export_folder");
		Options.bExternalizeInlineDetails = false;
		return UeAgentAuthoringFileIO::SaveObject(FilePath, Obj, Options, &OutError);
	}

	bool LoadAuthoringFile(const FString& FilePath, TSharedPtr<FJsonObject>& OutObj, TArray<TSharedPtr<FJsonValue>>& Issues, FString& OutError, const bool bOptional)
	{
		UeAgentAuthoringFileIO::FLoadOptions Options;
		Options.bOptional = bOptional;
		FString LoadError;
		const bool bOk = UeAgentAuthoringFileIO::LoadObject(FilePath, OutObj, Options, LoadError);
		if (!bOk)
		{
			OutError = LoadError.IsEmpty() ? TEXT("hjson_load_failed") : LoadError;
			UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("hjson_load_failed"), FilePath, OutError);
		}
		return bOk;
	}

	FString DefaultFolderForAsset(const FString& AssetPath)
	{
		FString SafeName = NormalizeAssetPath(AssetPath);
		SafeName.RemoveFromStart(TEXT("/"));
		SafeName.ReplaceInline(TEXT("/"), TEXT("__"));
		SafeName.ReplaceInline(TEXT("."), TEXT("_"));
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("ControlRig"), SafeName));
	}

	static TSharedPtr<FJsonObject> MakeVectorJson(const FVector& Value)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("x"), Value.X);
		Obj->SetNumberField(TEXT("y"), Value.Y);
		Obj->SetNumberField(TEXT("z"), Value.Z);
		return Obj;
	}

	static TSharedPtr<FJsonObject> MakeVector2DJson(const FVector2D& Value)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("x"), Value.X);
		Obj->SetNumberField(TEXT("y"), Value.Y);
		return Obj;
	}

	static TSharedPtr<FJsonObject> MakeRotatorJson(const FRotator& Value)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("pitch"), Value.Pitch);
		Obj->SetNumberField(TEXT("yaw"), Value.Yaw);
		Obj->SetNumberField(TEXT("roll"), Value.Roll);
		return Obj;
	}

	static TSharedPtr<FJsonObject> MakeColorJson(const FLinearColor& Value)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("r"), Value.R);
		Obj->SetNumberField(TEXT("g"), Value.G);
		Obj->SetNumberField(TEXT("b"), Value.B);
		Obj->SetNumberField(TEXT("a"), Value.A);
		return Obj;
	}

	TSharedPtr<FJsonObject> MakeTransformJson(const FTransform& Value)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetObjectField(TEXT("location"), MakeVectorJson(Value.GetLocation()));
		Obj->SetObjectField(TEXT("rotation"), MakeRotatorJson(Value.Rotator()));
		Obj->SetObjectField(TEXT("scale"), MakeVectorJson(Value.GetScale3D()));
		return Obj;
	}

	static bool ReadLinearColorField(
		const TSharedPtr<FJsonObject>& Obj,
		const FString& FieldName,
		const FString& Path,
		FLinearColor& OutValue,
		TArray<TSharedPtr<FJsonValue>>& Issues,
		const bool bRequired)
	{
		const TSharedPtr<FJsonValue>* FieldValue = UeAgentJsonDiagnostics::FindFieldValue(Obj, FieldName);
		if (!FieldValue)
		{
			if (bRequired)
			{
				UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("warning"), TEXT("json_missing_required_field"), Path, TEXT("Required color field is missing."));
			}
			return false;
		}

		const TSharedPtr<FJsonObject>* ColorObj = nullptr;
		if ((*FieldValue)->TryGetObject(ColorObj) && ColorObj && ColorObj->IsValid())
		{
			double R = 0.0;
			double G = 0.0;
			double B = 0.0;
			double A = 1.0;
			if (UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(*ColorObj, { TEXT("r"), TEXT("R"), TEXT("red"), TEXT("Red") }, R)
				&& UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(*ColorObj, { TEXT("g"), TEXT("G"), TEXT("green"), TEXT("Green") }, G)
				&& UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(*ColorObj, { TEXT("b"), TEXT("B"), TEXT("blue"), TEXT("Blue") }, B))
			{
				UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(*ColorObj, { TEXT("a"), TEXT("A"), TEXT("alpha"), TEXT("Alpha") }, A);
				OutValue = FLinearColor(R, G, B, A);
				return true;
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* ArrayPtr = nullptr;
		if ((*FieldValue)->TryGetArray(ArrayPtr) && ArrayPtr && ArrayPtr->Num() >= 3)
		{
			double R = 0.0;
			double G = 0.0;
			double B = 0.0;
			double A = 1.0;
			if (UeAgentJsonDiagnostics::TryReadNumberValue((*ArrayPtr)[0], R)
				&& UeAgentJsonDiagnostics::TryReadNumberValue((*ArrayPtr)[1], G)
				&& UeAgentJsonDiagnostics::TryReadNumberValue((*ArrayPtr)[2], B))
			{
				if (ArrayPtr->Num() > 3)
				{
					UeAgentJsonDiagnostics::TryReadNumberValue((*ArrayPtr)[3], A);
				}
				OutValue = FLinearColor(R, G, B, A);
				return true;
			}
		}

		UeAgentJsonDiagnostics::AddIssue(
			Issues,
			TEXT("warning"),
			TEXT("json_field_type_mismatch"),
			Path,
			FString::Printf(TEXT("Expected color object {r,g,b,a} or numeric array [r,g,b,a] but got %s."), *UeAgentJsonDiagnostics::JsonValueTypeToString(*FieldValue)));
		return false;
	}

	static bool ReadVector2DField(
		const TSharedPtr<FJsonObject>& Obj,
		const FString& FieldName,
		const FString& Path,
		FVector2D& OutValue,
		TArray<TSharedPtr<FJsonValue>>& Issues,
		const bool bRequired)
	{
		const TSharedPtr<FJsonValue>* FieldValue = UeAgentJsonDiagnostics::FindFieldValue(Obj, FieldName);
		if (!FieldValue)
		{
			if (bRequired)
			{
				UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("warning"), TEXT("json_missing_required_field"), Path, TEXT("Required vector2d field is missing."));
			}
			return false;
		}

		const TSharedPtr<FJsonObject>* ValueObj = nullptr;
		if ((*FieldValue)->TryGetObject(ValueObj) && ValueObj && ValueObj->IsValid())
		{
			double X = 0.0;
			double Y = 0.0;
			if (UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(*ValueObj, { TEXT("x"), TEXT("X") }, X)
				&& UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(*ValueObj, { TEXT("y"), TEXT("Y") }, Y))
			{
				OutValue = FVector2D(X, Y);
				return true;
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* ArrayPtr = nullptr;
		if ((*FieldValue)->TryGetArray(ArrayPtr) && ArrayPtr && ArrayPtr->Num() >= 2)
		{
			double X = 0.0;
			double Y = 0.0;
			if (UeAgentJsonDiagnostics::TryReadNumberValue((*ArrayPtr)[0], X)
				&& UeAgentJsonDiagnostics::TryReadNumberValue((*ArrayPtr)[1], Y))
			{
				OutValue = FVector2D(X, Y);
				return true;
			}
		}

		UeAgentJsonDiagnostics::AddIssue(
			Issues,
			TEXT("warning"),
			TEXT("json_field_type_mismatch"),
			Path,
			FString::Printf(TEXT("Expected vector2d object {x,y} or numeric array [x,y] but got %s."), *UeAgentJsonDiagnostics::JsonValueTypeToString(*FieldValue)));
		return false;
	}

	static bool ReadTransformField(
		const TSharedPtr<FJsonObject>& Obj,
		const FString& FieldName,
		const FString& Path,
		FTransform& OutValue,
		TArray<TSharedPtr<FJsonValue>>& Issues,
		const bool bRequired)
	{
		const TSharedPtr<FJsonValue>* FieldValue = UeAgentJsonDiagnostics::FindFieldValue(Obj, FieldName);
		if (!FieldValue)
		{
			if (bRequired)
			{
				UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("warning"), TEXT("json_missing_required_field"), Path, TEXT("Required transform field is missing."));
			}
			return false;
		}

		const TSharedPtr<FJsonObject>* TransformObj = nullptr;
		if (!(*FieldValue)->TryGetObject(TransformObj) || !TransformObj || !TransformObj->IsValid())
		{
			UeAgentJsonDiagnostics::AddIssue(
				Issues,
				TEXT("warning"),
				TEXT("json_field_type_mismatch"),
				Path,
				FString::Printf(TEXT("Expected transform object {location,rotation,scale} but got %s."), *UeAgentJsonDiagnostics::JsonValueTypeToString(*FieldValue)));
			return false;
		}

		FVector Location = OutValue.GetLocation();
		FRotator Rotation = OutValue.Rotator();
		FVector Scale = OutValue.GetScale3D();
		UeAgentJsonDiagnostics::ReadVectorField(*TransformObj, TEXT("location"), Path + TEXT(".location"), Location, Issues, false);
		UeAgentJsonDiagnostics::ReadRotatorField(*TransformObj, TEXT("rotation"), Path + TEXT(".rotation"), Rotation, Issues, false);
		UeAgentJsonDiagnostics::ReadVectorField(*TransformObj, TEXT("scale"), Path + TEXT(".scale"), Scale, Issues, false);
		OutValue = FTransform(Rotation, Location, Scale);
		return true;
	}

	static FString NumberDefaultText(const double Value)
	{
		FString Text = FString::SanitizeFloat(Value);
		if (Text == TEXT("-0"))
		{
			Text = TEXT("0");
		}
		return Text;
	}

	static FString BoolDefaultText(const bool bValue)
	{
		return bValue ? TEXT("True") : TEXT("False");
	}

	static FString QuoteDefaultString(const FString& InText)
	{
		FString Escaped = InText;
		Escaped.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""));
		return FString::Printf(TEXT("\"%s\""), *Escaped);
	}

	static FString VectorDefaultText(const FVector& Value)
	{
		return FString::Printf(
			TEXT("(X=%s,Y=%s,Z=%s)"),
			*NumberDefaultText(Value.X),
			*NumberDefaultText(Value.Y),
			*NumberDefaultText(Value.Z));
	}

	static FString RotatorDefaultText(const FRotator& Value)
	{
		return FString::Printf(
			TEXT("(Pitch=%s,Yaw=%s,Roll=%s)"),
			*NumberDefaultText(Value.Pitch),
			*NumberDefaultText(Value.Yaw),
			*NumberDefaultText(Value.Roll));
	}

	static FString QuatDefaultText(const FQuat& Value)
	{
		return FString::Printf(
			TEXT("(X=%s,Y=%s,Z=%s,W=%s)"),
			*NumberDefaultText(Value.X),
			*NumberDefaultText(Value.Y),
			*NumberDefaultText(Value.Z),
			*NumberDefaultText(Value.W));
	}

	static FString TransformDefaultText(const FTransform& Value)
	{
		return FString::Printf(
			TEXT("(Rotation=%s,Translation=%s,Scale3D=%s)"),
			*QuatDefaultText(Value.GetRotation()),
			*VectorDefaultText(Value.GetLocation()),
			*VectorDefaultText(Value.GetScale3D()));
	}

	static FString ArrayDefaultText(const TArray<FString>& Items)
	{
		return FString::Printf(TEXT("(%s)"), *FString::Join(Items, TEXT(",")));
	}

	static bool TryGetStringFieldAny(const TSharedPtr<FJsonObject>& Obj, const TArray<FString>& FieldNames, FString& OutValue)
	{
		if (!Obj.IsValid())
		{
			return false;
		}
		for (const FString& FieldName : FieldNames)
		{
			if (Obj->TryGetStringField(FieldName, OutValue))
			{
				OutValue.TrimStartAndEndInline();
				return true;
			}
		}
		return false;
	}

	static bool TryGetNumberFieldAny(const TSharedPtr<FJsonObject>& Obj, const TArray<FString>& FieldNames, double& OutValue)
	{
		if (!Obj.IsValid())
		{
			return false;
		}
		for (const FString& FieldName : FieldNames)
		{
			if (Obj->TryGetNumberField(FieldName, OutValue))
			{
				return true;
			}
		}
		return false;
	}

	static bool TryGetBoolFieldAny(const TSharedPtr<FJsonObject>& Obj, const TArray<FString>& FieldNames, bool& OutValue)
	{
		if (!Obj.IsValid())
		{
			return false;
		}
		for (const FString& FieldName : FieldNames)
		{
			if (Obj->TryGetBoolField(FieldName, OutValue))
			{
				return true;
			}
		}
		return false;
	}

	static FString NormalizePBIKLimitType(const FString& InText, const FString& DefaultValue = TEXT("EPBIKLimitType::Free"))
	{
		FString Text = InText.TrimStartAndEnd().ToLower();
		Text.ReplaceInline(TEXT("_"), TEXT(""));
		Text.ReplaceInline(TEXT("-"), TEXT(""));
		Text.RemoveFromStart(TEXT("epbiklimittype::"));
		if (Text == TEXT("free"))
		{
			return TEXT("EPBIKLimitType::Free");
		}
		if (Text == TEXT("limited") || Text == TEXT("limit"))
		{
			return TEXT("EPBIKLimitType::Limited");
		}
		if (Text == TEXT("locked") || Text == TEXT("lock"))
		{
			return TEXT("EPBIKLimitType::Locked");
		}
		return DefaultValue;
	}

	static FString NormalizeFBIKBoneLimitType(const FString& InText, const FString& DefaultValue = TEXT("EFBIKBoneLimitType::Free"))
	{
		FString Text = InText.TrimStartAndEnd().ToLower();
		Text.ReplaceInline(TEXT("_"), TEXT(""));
		Text.ReplaceInline(TEXT("-"), TEXT(""));
		Text.RemoveFromStart(TEXT("efbikbonelimittype::"));
		if (Text == TEXT("free"))
		{
			return TEXT("EFBIKBoneLimitType::Free");
		}
		if (Text == TEXT("limited") || Text == TEXT("limit"))
		{
			return TEXT("EFBIKBoneLimitType::Limit");
		}
		if (Text == TEXT("locked") || Text == TEXT("lock"))
		{
			return TEXT("EFBIKBoneLimitType::Locked");
		}
		return DefaultValue;
	}

	static FString NormalizePoleVectorOption(const FString& InText, const FString& DefaultValue = TEXT("EPoleVectorOption::Direction"))
	{
		FString Text = InText.TrimStartAndEnd().ToLower();
		Text.ReplaceInline(TEXT("_"), TEXT(""));
		Text.ReplaceInline(TEXT("-"), TEXT(""));
		Text.RemoveFromStart(TEXT("epolevectoroption::"));
		if (Text == TEXT("location") || Text == TEXT("position"))
		{
			return TEXT("EPoleVectorOption::Location");
		}
		if (Text == TEXT("direction") || Text == TEXT("dir"))
		{
			return TEXT("EPoleVectorOption::Direction");
		}
		return DefaultValue;
	}

	static bool TryReadBoneNameValue(const TSharedPtr<FJsonValue>& Value, FString& OutBoneName)
	{
		if (!Value.IsValid())
		{
			return false;
		}
		if (Value->TryGetString(OutBoneName))
		{
			OutBoneName.TrimStartAndEndInline();
			return !OutBoneName.IsEmpty();
		}
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (Value->TryGetObject(Obj) && Obj && Obj->IsValid())
		{
			return TryGetStringFieldAny(*Obj, { TEXT("bone"), TEXT("bone_name"), TEXT("name"), TEXT("item") }, OutBoneName) && !OutBoneName.IsEmpty();
		}
		return false;
	}

	static bool TryReadBoneNameField(const TSharedPtr<FJsonObject>& Obj, const FString& FieldName, FString& OutBoneName)
	{
		const TSharedPtr<FJsonValue>* FieldValue = UeAgentJsonDiagnostics::FindFieldValue(Obj, FieldName);
		return FieldValue && TryReadBoneNameValue(*FieldValue, OutBoneName);
	}

	static void SetStructuredPinDefault(TSharedPtr<FJsonObject> NodeObj, const FString& PinName, const FString& DefaultValue)
	{
		if (!NodeObj.IsValid())
		{
			return;
		}

		TSharedPtr<FJsonObject> PinDefaultsObj;
		const TSharedPtr<FJsonObject>* ExistingPinDefaults = nullptr;
		if (NodeObj->TryGetObjectField(TEXT("pin_defaults"), ExistingPinDefaults) && ExistingPinDefaults && ExistingPinDefaults->IsValid())
		{
			PinDefaultsObj = *ExistingPinDefaults;
		}
		else
		{
			PinDefaultsObj = MakeShared<FJsonObject>();
			NodeObj->SetObjectField(TEXT("pin_defaults"), PinDefaultsObj);
		}
		PinDefaultsObj->SetStringField(PinName, DefaultValue);
	}

#include "Domains/Rigging/UeAgentControlRigDomainBackend.inl"
}


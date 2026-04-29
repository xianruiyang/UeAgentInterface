// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_ControlRig.h"

#include "UeAgentJsonDiagnostics.h"

#include "AssetRegistry/AssetRegistryModule.h"
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

	template <typename T>
	static T* LoadAsset(const FString& InPath)
	{
		return Cast<T>(LoadAssetObject(InPath));
	}

	static UControlRigBlueprint* LoadControlRigBlueprint(const FString& InPath)
	{
		return LoadAsset<UControlRigBlueprint>(InPath);
	}

	static UControlRigShapeLibrary* LoadShapeLibrary(const FString& InPath)
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

	static FString ExportVariableDefaultValue(UControlRigBlueprint* Blueprint, const FBPVariableDescription& VarDesc)
	{
		if (Blueprint && Blueprint->GeneratedClass)
		{
			if (UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject())
			{
				if (FProperty* Property = FindFProperty<FProperty>(Blueprint->GeneratedClass, VarDesc.VarName))
				{
					if (void* ValuePtr = Property->ContainerPtrToValuePtr<void>(CDO))
					{
						FString ExportedValue;
						Property->ExportTextItem_Direct(ExportedValue, ValuePtr, ValuePtr, CDO, PPF_None);
						return ExportedValue;
					}
				}
			}
		}
		return VarDesc.DefaultValue;
	}

	static TSharedPtr<FJsonObject> BuildVariableJson(UControlRigBlueprint* Blueprint, const FBPVariableDescription& VarDesc)
	{
		TSharedPtr<FJsonObject> VarObj = MakeShared<FJsonObject>();
		VarObj->SetStringField(TEXT("name"), VarDesc.VarName.ToString());
		VarObj->SetObjectField(TEXT("type"), MakePinTypeJson(VarDesc.VarType));
		VarObj->SetStringField(TEXT("cpp_type"), PinTypeToCppType(VarDesc.VarType));
		VarObj->SetStringField(TEXT("default_value"), ExportVariableDefaultValue(Blueprint, VarDesc));
		VarObj->SetBoolField(TEXT("instance_editable"), (VarDesc.PropertyFlags & CPF_DisableEditOnInstance) == 0);
		VarObj->SetBoolField(TEXT("read_only"), (VarDesc.PropertyFlags & CPF_BlueprintReadOnly) != 0);
		return VarObj;
	}

	static TSharedPtr<FJsonObject> BuildVariablesJson(UControlRigBlueprint* Blueprint)
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
		return Root;
	}

	static bool ReadJsonValueAsDefaultText(const TSharedPtr<FJsonValue>& Value, FString& OutText)
	{
		if (!Value.IsValid() || Value->IsNull())
		{
			OutText.Reset();
			return true;
		}
		FString StringValue;
		if (Value->TryGetString(StringValue))
		{
			OutText = StringValue;
			return true;
		}
		double NumberValue = 0.0;
		if (Value->TryGetNumber(NumberValue))
		{
			OutText = FString::SanitizeFloat(NumberValue);
			return true;
		}
		bool BoolValue = false;
		if (Value->TryGetBool(BoolValue))
		{
			OutText = BoolValue ? TEXT("true") : TEXT("false");
			return true;
		}
		const TSharedPtr<FJsonObject>* ObjectValue = nullptr;
		if (Value->TryGetObject(ObjectValue) && ObjectValue && ObjectValue->IsValid())
		{
			double X = 0.0;
			double Y = 0.0;
			double Z = 0.0;
			double A = 1.0;
			if ((*ObjectValue)->TryGetNumberField(TEXT("x"), X) && (*ObjectValue)->TryGetNumberField(TEXT("y"), Y) && (*ObjectValue)->TryGetNumberField(TEXT("z"), Z))
			{
				OutText = FString::Printf(TEXT("(X=%s,Y=%s,Z=%s)"), *FString::SanitizeFloat(X), *FString::SanitizeFloat(Y), *FString::SanitizeFloat(Z));
				return true;
			}
			if ((*ObjectValue)->TryGetNumberField(TEXT("r"), X) && (*ObjectValue)->TryGetNumberField(TEXT("g"), Y) && (*ObjectValue)->TryGetNumberField(TEXT("b"), Z))
			{
				(*ObjectValue)->TryGetNumberField(TEXT("a"), A);
				OutText = FString::Printf(TEXT("(R=%s,G=%s,B=%s,A=%s)"), *FString::SanitizeFloat(X), *FString::SanitizeFloat(Y), *FString::SanitizeFloat(Z), *FString::SanitizeFloat(A));
				return true;
			}
		}
		return false;
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

	struct FControlRigVariableApplyStats
	{
		int32 Added = 0;
		int32 DefaultsUpdated = 0;
		int32 FlagsUpdated = 0;
		int32 Existing = 0;
	};

	static void ApplyVariablesJson(UControlRigBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Obj, const FString& Path, TArray<TSharedPtr<FJsonValue>>& Issues, FControlRigVariableApplyStats& Stats)
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

		for (int32 VariableIndex = 0; VariableIndex < Variables->Num(); ++VariableIndex)
		{
			TSharedPtr<FJsonObject> VarObj;
			const FString VarPath = FString::Printf(TEXT("%s.variables[%d]"), *Path, VariableIndex);
			if (!UeAgentJsonDiagnostics::ReadObjectFromValue((*Variables)[VariableIndex], VarPath, VarObj, Issues))
			{
				continue;
			}
			UeAgentJsonDiagnostics::WarnUnknownFields(VarObj, VarPath, { TEXT("name"), TEXT("cpp_type"), TEXT("type"), TEXT("default_value"), TEXT("direction"), TEXT("expose_to_anim_instance"), TEXT("instance_editable"), TEXT("read_only"), TEXT("category"), TEXT("metadata") }, Issues);

			FString Name;
			if (!UeAgentJsonDiagnostics::ReadStringField(VarObj, TEXT("name"), VarPath + TEXT(".name"), Name, Issues, true) || Name.TrimStartAndEnd().IsEmpty())
			{
				continue;
			}
			Name.TrimStartAndEndInline();

			FEdGraphPinType PinType;
			FString TypeError;
			if (!BuildPinTypeFromVariableJson(VarObj, PinType, TypeError))
			{
				UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TypeError, VarPath + TEXT(".type"), FString::Printf(TEXT("Invalid Control Rig variable type for '%s'."), *Name));
				continue;
			}

			FString DefaultValue;
			if (const TSharedPtr<FJsonValue>* DefaultValueJson = UeAgentJsonDiagnostics::FindFieldValue(VarObj, TEXT("default_value")))
			{
				if (!ReadJsonValueAsDefaultText(*DefaultValueJson, DefaultValue))
				{
					UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("invalid_variable_default_value"), VarPath + TEXT(".default_value"), TEXT("default_value must be a string, number, bool, vector object, or color object."));
					continue;
				}
			}

			const FName VariableName(*Name);
			const int32 ExistingIndex = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VariableName);
			if (ExistingIndex == INDEX_NONE)
			{
				if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, VariableName, PinType, DefaultValue))
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
			if (!DefaultValue.IsEmpty())
			{
				if (!Blueprint->GeneratedClass || !FindFProperty<FProperty>(Blueprint->GeneratedClass, VariableName))
				{
					FCompilerResultsLog LocalCompileLog;
					FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &LocalCompileLog);
				}
				if (Blueprint->GeneratedClass)
				{
					if (FProperty* Property = FindFProperty<FProperty>(Blueprint->GeneratedClass, VariableName))
					{
						UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject();
						void* ValuePtr = CDO ? Property->ContainerPtrToValuePtr<void>(CDO) : nullptr;
						if (ValuePtr && Property->ImportText_Direct(*DefaultValue, ValuePtr, CDO, PPF_None) != nullptr)
						{
							if (const int32 NewIndex = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VariableName); NewIndex != INDEX_NONE)
							{
								Blueprint->NewVariables[NewIndex].DefaultValue = DefaultValue;
							}
							++Stats.DefaultsUpdated;
						}
						else
						{
							UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("control_rig_variable_default_import_failed"), VarPath + TEXT(".default_value"), FString::Printf(TEXT("Failed to import default value for variable '%s'."), *Name));
						}
					}
				}
			}
		}
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

	static bool HasBlockingIssue(const TArray<TSharedPtr<FJsonValue>>& Issues)
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

	static int32 CountIssuesBySeverity(const TArray<TSharedPtr<FJsonValue>>& Issues, const FString& SeverityToMatch)
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
		const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&OutJson);
		return FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
	}

	static bool SaveJsonFile(const FString& FilePath, const TSharedPtr<FJsonObject>& Obj, FString& OutError)
	{
		FString JsonText;
		if (!MakeJsonString(Obj, JsonText))
		{
			OutError = TEXT("serialize_json_failed");
			return false;
		}

		const FString ResolvedFilePath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(FilePath);
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(ResolvedFilePath), true);
		if (!FFileHelper::SaveStringToFile(JsonText, *ResolvedFilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = FString::Printf(TEXT("save_json_failed:%s"), *ResolvedFilePath);
			return false;
		}
		return true;
	}

	static bool LoadJsonFile(const FString& FilePath, TSharedPtr<FJsonObject>& OutObj, TArray<TSharedPtr<FJsonValue>>& Issues, FString& OutError, const bool bOptional = false)
	{
		return UeAgentJsonDiagnostics::LoadJsonObjectFile(FilePath, OutObj, &Issues, OutError, bOptional);
	}

	static FString DefaultFolderForAsset(const FString& AssetPath)
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

	static TSharedPtr<FJsonObject> MakeTransformJson(const FTransform& Value)
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

	template <typename TEnum>
	static FString EnumToString(const TEnum Value)
	{
		if (const UEnum* Enum = StaticEnum<TEnum>())
		{
			return Enum->GetNameStringByValue(static_cast<int64>(Value));
		}
		return FString::FromInt(static_cast<int32>(Value));
	}

	template <typename TEnum>
	static bool ParseEnumString(const FString& InText, TEnum& OutValue)
	{
		const UEnum* Enum = StaticEnum<TEnum>();
		if (!Enum)
		{
			return false;
		}

		FString Text = InText.TrimStartAndEnd();
		int64 Value = Enum->GetValueByNameString(Text);
		if (Value == INDEX_NONE)
		{
			const FString EnumName = Enum->GetName();
			if (!Text.StartsWith(EnumName + TEXT("::")))
			{
				Value = Enum->GetValueByNameString(EnumName + TEXT("::") + Text);
			}
		}

		if (Value == INDEX_NONE)
		{
			for (int32 Index = 0; Index < Enum->NumEnums() - 1; ++Index)
			{
				if (Enum->GetNameStringByIndex(Index).Equals(Text, ESearchCase::IgnoreCase))
				{
					Value = Enum->GetValueByIndex(Index);
					break;
				}
			}
		}

		if (Value == INDEX_NONE)
		{
			return false;
		}

		OutValue = static_cast<TEnum>(Value);
		return true;
	}

	static ERigElementType ParseElementType(const FString& InText, const ERigElementType DefaultType)
	{
		FString Text = InText.TrimStartAndEnd().ToLower();
		Text.ReplaceInline(TEXT("_"), TEXT(""));
		Text.ReplaceInline(TEXT("-"), TEXT(""));
		if (Text == TEXT("bone"))
		{
			return ERigElementType::Bone;
		}
		if (Text == TEXT("null"))
		{
			return ERigElementType::Null;
		}
		if (Text == TEXT("control"))
		{
			return ERigElementType::Control;
		}
		if (Text == TEXT("curve"))
		{
			return ERigElementType::Curve;
		}
		if (Text == TEXT("connector"))
		{
			return ERigElementType::Connector;
		}
		if (Text == TEXT("socket"))
		{
			return ERigElementType::Socket;
		}
		return DefaultType;
	}

	static FString ElementTypeToString(const ERigElementType Type)
	{
		switch (Type)
		{
		case ERigElementType::Bone: return TEXT("Bone");
		case ERigElementType::Null: return TEXT("Null");
		case ERigElementType::Control: return TEXT("Control");
		case ERigElementType::Curve: return TEXT("Curve");
		case ERigElementType::Connector: return TEXT("Connector");
		case ERigElementType::Socket: return TEXT("Socket");
		default: return TEXT("None");
		}
	}

	static FRigElementKey ReadParentKey(const TSharedPtr<FJsonObject>& Obj, const ERigElementType DefaultType)
	{
		const TSharedPtr<FJsonValue>* ParentValue = UeAgentJsonDiagnostics::FindFieldValue(Obj, TEXT("parent"));
		if (!ParentValue)
		{
			FString ParentName;
			if (Obj->TryGetStringField(TEXT("parent_name"), ParentName) && !ParentName.IsEmpty())
			{
				FString ParentType;
				Obj->TryGetStringField(TEXT("parent_type"), ParentType);
				return FRigElementKey(FName(*ParentName), ParseElementType(ParentType, DefaultType));
			}
			return FRigElementKey();
		}

		FString ParentString;
		if ((*ParentValue)->TryGetString(ParentString) && !ParentString.IsEmpty())
		{
			return FRigElementKey(FName(*ParentString), DefaultType);
		}

		const TSharedPtr<FJsonObject>* ParentObj = nullptr;
		if ((*ParentValue)->TryGetObject(ParentObj) && ParentObj && ParentObj->IsValid())
		{
			FString ParentName;
			FString ParentType;
			(*ParentObj)->TryGetStringField(TEXT("name"), ParentName);
			(*ParentObj)->TryGetStringField(TEXT("type"), ParentType);
			if (!ParentName.IsEmpty())
			{
				return FRigElementKey(FName(*ParentName), ParseElementType(ParentType, DefaultType));
			}
		}
		return FRigElementKey();
	}

	static TSharedPtr<FJsonObject> BuildParentJson(const URigHierarchy* Hierarchy, const FRigElementKey& Key)
	{
		TSharedPtr<FJsonObject> ParentObj = MakeShared<FJsonObject>();
		if (!Hierarchy)
		{
			return ParentObj;
		}

		const FRigElementKey ParentKey = Hierarchy->GetFirstParent(Key);
		if (ParentKey.IsValid())
		{
			ParentObj->SetStringField(TEXT("name"), ParentKey.Name.ToString());
			ParentObj->SetStringField(TEXT("type"), ElementTypeToString(ParentKey.Type));
		}
		return ParentObj;
	}

	static TSharedPtr<FJsonObject> BuildShapeDefinitionJson(const FControlRigShapeDefinition& Shape)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Shape.ShapeName.ToString());
		Obj->SetStringField(TEXT("shape_name"), Shape.ShapeName.ToString());
		Obj->SetStringField(TEXT("static_mesh"), Shape.StaticMesh.ToSoftObjectPath().ToString());
		Obj->SetObjectField(TEXT("transform"), MakeTransformJson(Shape.Transform));
		return Obj;
	}

	static bool ApplyShapeDefinitionJson(
		const TSharedPtr<FJsonObject>& Obj,
		const FString& Path,
		FControlRigShapeDefinition& InOutShape,
		TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		if (!Obj.IsValid())
		{
			UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("warning"), TEXT("json_field_type_mismatch"), Path, TEXT("Expected shape definition object."));
			return false;
		}

		UeAgentJsonDiagnostics::WarnUnknownFields(Obj, Path, { TEXT("name"), TEXT("shape_name"), TEXT("static_mesh"), TEXT("transform") }, Issues);

		FString ShapeName;
		if (Obj->TryGetStringField(TEXT("shape_name"), ShapeName) || Obj->TryGetStringField(TEXT("name"), ShapeName))
		{
			if (ShapeName.TrimStartAndEnd().IsEmpty())
			{
				UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("warning"), TEXT("json_empty_required_field"), Path + TEXT(".shape_name"), TEXT("Shape name cannot be empty."));
			}
			else
			{
				InOutShape.ShapeName = FName(*ShapeName);
			}
		}

		FString StaticMeshPath;
		if (Obj->TryGetStringField(TEXT("static_mesh"), StaticMeshPath))
		{
			if (StaticMeshPath.TrimStartAndEnd().IsEmpty())
			{
				InOutShape.StaticMesh = nullptr;
			}
			else if (UStaticMesh* StaticMesh = LoadAsset<UStaticMesh>(StaticMeshPath))
			{
				InOutShape.StaticMesh = StaticMesh;
			}
			else
			{
				UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("warning"), TEXT("asset_reference_not_found"), Path + TEXT(".static_mesh"), FString::Printf(TEXT("Static mesh '%s' could not be loaded."), *StaticMeshPath));
			}
		}

		FTransform ShapeTransform = InOutShape.Transform;
		if (ReadTransformField(Obj, TEXT("transform"), Path + TEXT(".transform"), ShapeTransform, Issues, false))
		{
			InOutShape.Transform = ShapeTransform;
		}
		return !HasBlockingIssue(Issues);
	}

	static TSharedPtr<FJsonObject> BuildShapeLibraryInfo(UControlRigShapeLibrary* Library)
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		if (!Library)
		{
			return Out;
		}

		const FString AssetPath = NormalizeAssetPath(Library->GetPathName());
		Out->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.control_rig_shape_library.v1"));
		Out->SetStringField(TEXT("asset_path"), AssetPath);
		Out->SetStringField(TEXT("object_path"), Library->GetPathName());
		Out->SetStringField(TEXT("asset_class"), Library->GetClass()->GetPathName());
		Out->SetObjectField(TEXT("default_shape"), BuildShapeDefinitionJson(Library->DefaultShape));
		Out->SetStringField(TEXT("default_material"), Library->DefaultMaterial.ToSoftObjectPath().ToString());
		Out->SetStringField(TEXT("xray_material"), Library->XRayMaterial.ToSoftObjectPath().ToString());
		Out->SetStringField(TEXT("material_color_parameter"), Library->MaterialColorParameter.ToString());

		TArray<TSharedPtr<FJsonValue>> ShapeValues;
		for (const FControlRigShapeDefinition& Shape : Library->Shapes)
		{
			ShapeValues.Add(MakeShared<FJsonValueObject>(BuildShapeDefinitionJson(Shape)));
		}
		Out->SetArrayField(TEXT("shapes"), ShapeValues);
		Out->SetNumberField(TEXT("shape_count"), ShapeValues.Num());
		return Out;
	}

	static UControlRigShapeLibrary* CreateShapeLibraryAsset(const FString& AssetPath, FString& OutError)
	{
		const FString NormalizedAssetPath = NormalizeAssetPath(AssetPath);
		if (!FPackageName::IsValidLongPackageName(NormalizedAssetPath))
		{
			OutError = TEXT("invalid_asset_path");
			return nullptr;
		}
		if (LoadAssetObject(NormalizedAssetPath))
		{
			OutError = TEXT("asset_already_exists");
			return nullptr;
		}

		const FString AssetName = FPackageName::GetLongPackageAssetName(NormalizedAssetPath);
		UPackage* Package = CreatePackage(*NormalizedAssetPath);
		if (!Package)
		{
			OutError = TEXT("create_package_failed");
			return nullptr;
		}

		UControlRigShapeLibrary* Library = NewObject<UControlRigShapeLibrary>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
		if (!Library)
		{
			OutError = TEXT("create_asset_failed");
			return nullptr;
		}

		FAssetRegistryModule::AssetCreated(Library);
		Package->MarkPackageDirty();
		return Library;
	}

	static UControlRigBlueprint* CreateControlRigAsset(
		const FString& AssetPath,
		const bool bModularRig,
		const FString& PreviewSkeletalMeshPath,
		const bool bImportHierarchyFromPreview,
		FString& OutError)
	{
		const FString NormalizedAssetPath = NormalizeAssetPath(AssetPath);
		if (!FPackageName::IsValidLongPackageName(NormalizedAssetPath))
		{
			OutError = TEXT("invalid_asset_path");
			return nullptr;
		}
		if (LoadAssetObject(NormalizedAssetPath))
		{
			OutError = TEXT("asset_already_exists");
			return nullptr;
		}

		UControlRigBlueprint* Blueprint = UControlRigBlueprintFactory::CreateNewControlRigAsset(NormalizedAssetPath, bModularRig);
		if (!Blueprint)
		{
			OutError = TEXT("create_control_rig_failed");
			return nullptr;
		}

		if (!PreviewSkeletalMeshPath.TrimStartAndEnd().IsEmpty())
		{
			USkeletalMesh* PreviewMesh = LoadAsset<USkeletalMesh>(PreviewSkeletalMeshPath);
			if (!PreviewMesh)
			{
				OutError = TEXT("preview_skeletal_mesh_not_found");
				return nullptr;
			}
			Blueprint->SetPreviewMesh(PreviewMesh, true);
			if (bImportHierarchyFromPreview && Blueprint->GetHierarchyController())
			{
				Blueprint->GetHierarchyController()->ImportBonesFromSkeletalMesh(PreviewMesh, NAME_None, true, true, false, false, false);
			}
		}

		Blueprint->MarkPackageDirty();
		return Blueprint;
	}

	static FString ControlRigTypeToString(const EControlRigType Type)
	{
		switch (Type)
		{
		case EControlRigType::IndependentRig: return TEXT("independent_rig");
		case EControlRigType::RigModule: return TEXT("rig_module");
		case EControlRigType::ModularRig: return TEXT("modular_rig");
		default: return TEXT("invalid");
		}
	}

	static bool ParseControlRigTypeText(const FString& InText, EControlRigType& OutType)
	{
		FString Text = InText.TrimStartAndEnd().ToLower();
		Text.ReplaceInline(TEXT("-"), TEXT("_"));
		if (Text.IsEmpty() || Text == TEXT("independent") || Text == TEXT("independent_rig") || Text == TEXT("standalone"))
		{
			OutType = EControlRigType::IndependentRig;
			return true;
		}
		if (Text == TEXT("rig_module") || Text == TEXT("module"))
		{
			OutType = EControlRigType::RigModule;
			return true;
		}
		if (Text == TEXT("modular") || Text == TEXT("modular_rig"))
		{
			OutType = EControlRigType::ModularRig;
			return true;
		}
		return false;
	}

	static TSharedPtr<FJsonObject> BuildControlSettingsJson(const FRigControlSettings& Settings)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("animation_type"), EnumToString(Settings.AnimationType));
		Obj->SetStringField(TEXT("control_type"), EnumToString(Settings.ControlType));
		Obj->SetStringField(TEXT("display_name"), Settings.DisplayName.ToString());
		Obj->SetStringField(TEXT("primary_axis"), EnumToString(Settings.PrimaryAxis));
		Obj->SetBoolField(TEXT("shape_visible"), Settings.bShapeVisible);
		Obj->SetStringField(TEXT("shape_visibility"), EnumToString(Settings.ShapeVisibility));
		Obj->SetStringField(TEXT("shape_name"), Settings.ShapeName.ToString());
		Obj->SetObjectField(TEXT("shape_color"), MakeColorJson(Settings.ShapeColor));
		Obj->SetBoolField(TEXT("transient_control"), Settings.bIsTransientControl);
		Obj->SetBoolField(TEXT("group_with_parent_control"), Settings.bGroupWithParentControl);
		Obj->SetBoolField(TEXT("restrict_space_switching"), Settings.bRestrictSpaceSwitching);
		Obj->SetBoolField(TEXT("use_preferred_rotation_order"), Settings.bUsePreferredRotationOrder);
		return Obj;
	}

	static void ApplyControlSettingsJson(
		const TSharedPtr<FJsonObject>& Obj,
		const FString& Path,
		FRigControlSettings& InOutSettings,
		TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		if (!Obj.IsValid())
		{
			return;
		}

		UeAgentJsonDiagnostics::WarnUnknownFields(
			Obj,
			Path,
			{
				TEXT("animation_type"), TEXT("control_type"), TEXT("display_name"), TEXT("primary_axis"),
				TEXT("shape_visible"), TEXT("shape_visibility"), TEXT("shape_name"), TEXT("shape_color"),
				TEXT("transient_control"), TEXT("group_with_parent_control"), TEXT("restrict_space_switching"),
				TEXT("use_preferred_rotation_order")
			},
			Issues);

		FString TextValue;
		if (Obj->TryGetStringField(TEXT("animation_type"), TextValue))
		{
			ERigControlAnimationType Parsed = InOutSettings.AnimationType;
			if (ParseEnumString(TextValue, Parsed))
			{
				InOutSettings.AnimationType = Parsed;
			}
			else
			{
				UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("warning"), TEXT("json_enum_parse_failed"), Path + TEXT(".animation_type"), FString::Printf(TEXT("Unknown ERigControlAnimationType '%s'."), *TextValue));
			}
		}
		if (Obj->TryGetStringField(TEXT("control_type"), TextValue))
		{
			ERigControlType Parsed = InOutSettings.ControlType;
			if (ParseEnumString(TextValue, Parsed))
			{
				InOutSettings.ControlType = Parsed;
			}
			else
			{
				UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("warning"), TEXT("json_enum_parse_failed"), Path + TEXT(".control_type"), FString::Printf(TEXT("Unknown ERigControlType '%s'."), *TextValue));
			}
		}
		if (Obj->TryGetStringField(TEXT("display_name"), TextValue))
		{
			InOutSettings.DisplayName = FName(*TextValue);
		}
		if (Obj->TryGetStringField(TEXT("primary_axis"), TextValue))
		{
			ERigControlAxis Parsed = InOutSettings.PrimaryAxis;
			if (ParseEnumString(TextValue, Parsed))
			{
				InOutSettings.PrimaryAxis = Parsed;
			}
			else
			{
				UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("warning"), TEXT("json_enum_parse_failed"), Path + TEXT(".primary_axis"), FString::Printf(TEXT("Unknown ERigControlAxis '%s'."), *TextValue));
			}
		}
		bool bBool = false;
		if (UeAgentJsonDiagnostics::ReadBoolField(Obj, TEXT("shape_visible"), Path + TEXT(".shape_visible"), bBool, Issues, false))
		{
			InOutSettings.bShapeVisible = bBool;
		}
		if (Obj->TryGetStringField(TEXT("shape_visibility"), TextValue))
		{
			ERigControlVisibility Parsed = InOutSettings.ShapeVisibility;
			if (ParseEnumString(TextValue, Parsed))
			{
				InOutSettings.ShapeVisibility = Parsed;
			}
			else
			{
				UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("warning"), TEXT("json_enum_parse_failed"), Path + TEXT(".shape_visibility"), FString::Printf(TEXT("Unknown ERigControlVisibility '%s'."), *TextValue));
			}
		}
		if (Obj->TryGetStringField(TEXT("shape_name"), TextValue))
		{
			InOutSettings.ShapeName = FName(*TextValue);
		}
		FLinearColor Color = InOutSettings.ShapeColor;
		if (ReadLinearColorField(Obj, TEXT("shape_color"), Path + TEXT(".shape_color"), Color, Issues, false))
		{
			InOutSettings.ShapeColor = Color;
		}
		if (UeAgentJsonDiagnostics::ReadBoolField(Obj, TEXT("transient_control"), Path + TEXT(".transient_control"), bBool, Issues, false))
		{
			InOutSettings.bIsTransientControl = bBool;
		}
		if (UeAgentJsonDiagnostics::ReadBoolField(Obj, TEXT("group_with_parent_control"), Path + TEXT(".group_with_parent_control"), bBool, Issues, false))
		{
			InOutSettings.bGroupWithParentControl = bBool;
		}
		if (UeAgentJsonDiagnostics::ReadBoolField(Obj, TEXT("restrict_space_switching"), Path + TEXT(".restrict_space_switching"), bBool, Issues, false))
		{
			InOutSettings.bRestrictSpaceSwitching = bBool;
		}
		if (UeAgentJsonDiagnostics::ReadBoolField(Obj, TEXT("use_preferred_rotation_order"), Path + TEXT(".use_preferred_rotation_order"), bBool, Issues, false))
		{
			InOutSettings.bUsePreferredRotationOrder = bBool;
		}
		InOutSettings.SetupLimitArrayForType();
	}

	static TSharedPtr<FJsonObject> BuildRigElementJson(const URigHierarchy* Hierarchy, const FRigElementKey& Key)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Key.Name.ToString());
		Obj->SetStringField(TEXT("type"), ElementTypeToString(Key.Type));
		Obj->SetObjectField(TEXT("parent"), BuildParentJson(Hierarchy, Key));
		Obj->SetObjectField(TEXT("initial_local_transform"), MakeTransformJson(Hierarchy ? Hierarchy->GetLocalTransform(Key, true) : FTransform::Identity));
		Obj->SetObjectField(TEXT("current_local_transform"), MakeTransformJson(Hierarchy ? Hierarchy->GetLocalTransform(Key, false) : FTransform::Identity));
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildControlJson(const URigHierarchy* Hierarchy, const FRigControlElement* Control)
	{
		TSharedPtr<FJsonObject> Obj = BuildRigElementJson(Hierarchy, Control ? Control->GetKey() : FRigElementKey());
		if (!Hierarchy || !Control)
		{
			return Obj;
		}

		Obj->SetObjectField(TEXT("settings"), BuildControlSettingsJson(Control->Settings));
		Obj->SetObjectField(TEXT("value_transform"), MakeTransformJson(Hierarchy->GetControlValue(Control->GetKey(), ERigControlValueType::Initial).GetAsTransform(Control->Settings.ControlType, Control->Settings.PrimaryAxis)));
		Obj->SetObjectField(TEXT("offset_transform"), MakeTransformJson(Hierarchy->GetLocalTransform(Control->GetKey(), true)));
		Obj->SetObjectField(TEXT("shape_transform"), MakeTransformJson(Control->Settings.ShapeTransform));
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildGraphJson(URigVMGraph* Graph)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!Graph)
		{
			return Obj;
		}

		Obj->SetStringField(TEXT("name"), Graph->GetNodePath());
		Obj->SetBoolField(TEXT("default_graph"), Graph == Graph->GetRootGraph());

		TArray<TSharedPtr<FJsonValue>> NodeValues;
		for (URigVMNode* Node : Graph->GetNodes())
		{
			if (!Node)
			{
				continue;
			}

			TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
			NodeObj->SetStringField(TEXT("name"), Node->GetName());
			NodeObj->SetStringField(TEXT("title"), Node->GetNodeTitle());
			NodeObj->SetObjectField(TEXT("position"), MakeVector2DJson(Node->GetPosition()));

			if (const URigVMUnitNode* UnitNode = Cast<URigVMUnitNode>(Node))
			{
				NodeObj->SetStringField(TEXT("node_type"), TEXT("unit"));
				NodeObj->SetStringField(TEXT("script_struct"), UnitNode->GetScriptStruct() ? UnitNode->GetScriptStruct()->GetPathName() : FString());
				NodeObj->SetStringField(TEXT("method"), UnitNode->GetMethodName().ToString());
			}
			else if (const URigVMVariableNode* VariableNode = Cast<URigVMVariableNode>(Node))
			{
				NodeObj->SetStringField(TEXT("node_type"), TEXT("variable"));
				NodeObj->SetStringField(TEXT("variable_name"), VariableNode->GetVariableName().ToString());
				NodeObj->SetStringField(TEXT("cpp_type"), VariableNode->GetCPPType());
				NodeObj->SetStringField(TEXT("default_value"), VariableNode->GetDefaultValue());
			}
			else if (const URigVMTemplateNode* TemplateNode = Cast<URigVMTemplateNode>(Node))
			{
				NodeObj->SetStringField(TEXT("node_type"), TEXT("template"));
				NodeObj->SetStringField(TEXT("notation"), TemplateNode->GetNotation().ToString());
			}
			else
			{
				NodeObj->SetStringField(TEXT("node_type"), Node->GetClass()->GetName());
			}

			TSharedPtr<FJsonObject> PinDefaults = MakeShared<FJsonObject>();
			for (URigVMPin* Pin : Node->GetAllPinsRecursively())
			{
				if (Pin && !Pin->GetDefaultValue().IsEmpty())
				{
					PinDefaults->SetStringField(Pin->GetPinPath(false), Pin->GetDefaultValue());
				}
			}
			NodeObj->SetObjectField(TEXT("pin_defaults"), PinDefaults);
			NodeValues.Add(MakeShared<FJsonValueObject>(NodeObj));
		}
		Obj->SetArrayField(TEXT("nodes"), NodeValues);

		TArray<TSharedPtr<FJsonValue>> LinkValues;
		for (URigVMLink* Link : Graph->GetLinks())
		{
			if (!Link)
			{
				continue;
			}

			TSharedPtr<FJsonObject> LinkObj = MakeShared<FJsonObject>();
			LinkObj->SetStringField(TEXT("source"), Link->GetSourcePinPath());
			LinkObj->SetStringField(TEXT("target"), Link->GetTargetPinPath());
			LinkValues.Add(MakeShared<FJsonValueObject>(LinkObj));
		}
		Obj->SetArrayField(TEXT("links"), LinkValues);
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildControlRigInfo(UControlRigBlueprint* Blueprint)
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		if (!Blueprint)
		{
			return Out;
		}

		const FString AssetPath = NormalizeAssetPath(Blueprint->GetPathName());
		Out->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.control_rig.folder.v1"));
		Out->SetStringField(TEXT("asset_path"), AssetPath);
		Out->SetStringField(TEXT("object_path"), Blueprint->GetPathName());
		Out->SetStringField(TEXT("asset_class"), Blueprint->GetClass()->GetPathName());
		Out->SetStringField(TEXT("control_rig_type"), ControlRigTypeToString(Blueprint->ControlRigType));
		Out->SetStringField(TEXT("control_rig_class"), Blueprint->GetControlRigClass() ? Blueprint->GetControlRigClass()->GetPathName() : FString());
		Out->SetStringField(TEXT("generated_class"), Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetPathName() : FString());
		Out->SetStringField(TEXT("preview_skeletal_mesh"), Blueprint->GetPreviewMesh() ? Blueprint->GetPreviewMesh()->GetPathName() : FString());
		Out->SetNumberField(TEXT("blueprint_status"), static_cast<int32>(Blueprint->Status));

		TArray<TSharedPtr<FJsonValue>> ShapeLibraryValues;
		for (const TSoftObjectPtr<UControlRigShapeLibrary>& ShapeLibrary : Blueprint->ShapeLibraries)
		{
			ShapeLibraryValues.Add(MakeShared<FJsonValueString>(ShapeLibrary.ToSoftObjectPath().ToString()));
		}
		Out->SetArrayField(TEXT("shape_libraries"), ShapeLibraryValues);

		URigHierarchy* Hierarchy = Blueprint->GetHierarchy();
		TSharedPtr<FJsonObject> HierarchyObj = MakeShared<FJsonObject>();
		if (Hierarchy)
		{
			TArray<TSharedPtr<FJsonValue>> BoneValues;
			for (FRigBoneElement* Bone : Hierarchy->GetBones(true))
			{
				if (Bone)
				{
					TSharedPtr<FJsonObject> BoneObj = BuildRigElementJson(Hierarchy, Bone->GetKey());
					BoneObj->SetStringField(TEXT("bone_type"), EnumToString(Bone->BoneType));
					BoneValues.Add(MakeShared<FJsonValueObject>(BoneObj));
				}
			}
			HierarchyObj->SetArrayField(TEXT("bones"), BoneValues);
			HierarchyObj->SetNumberField(TEXT("bone_count"), BoneValues.Num());

			TArray<TSharedPtr<FJsonValue>> NullValues;
			for (FRigNullElement* Null : Hierarchy->GetNulls(true))
			{
				if (Null)
				{
					NullValues.Add(MakeShared<FJsonValueObject>(BuildRigElementJson(Hierarchy, Null->GetKey())));
				}
			}
			HierarchyObj->SetArrayField(TEXT("nulls"), NullValues);
			HierarchyObj->SetNumberField(TEXT("null_count"), NullValues.Num());

			TArray<TSharedPtr<FJsonValue>> ControlValues;
			for (FRigControlElement* Control : Hierarchy->GetControls(true))
			{
				if (Control)
				{
					ControlValues.Add(MakeShared<FJsonValueObject>(BuildControlJson(Hierarchy, Control)));
				}
			}
			HierarchyObj->SetArrayField(TEXT("controls"), ControlValues);
			HierarchyObj->SetNumberField(TEXT("control_count"), ControlValues.Num());

			TArray<TSharedPtr<FJsonValue>> CurveValues;
			for (FRigCurveElement* Curve : Hierarchy->GetCurves())
			{
				if (Curve)
				{
					TSharedPtr<FJsonObject> CurveObj = MakeShared<FJsonObject>();
					CurveObj->SetStringField(TEXT("name"), Curve->GetName());
					CurveObj->SetNumberField(TEXT("value"), Hierarchy->GetCurveValue(Curve->GetKey()));
					CurveValues.Add(MakeShared<FJsonValueObject>(CurveObj));
				}
			}
			HierarchyObj->SetArrayField(TEXT("curves"), CurveValues);
			HierarchyObj->SetNumberField(TEXT("curve_count"), CurveValues.Num());
		}
		Out->SetObjectField(TEXT("hierarchy"), HierarchyObj);

		TArray<TSharedPtr<FJsonValue>> GraphValues;
		for (URigVMGraph* Graph : Blueprint->GetAllModels())
		{
			GraphValues.Add(MakeShared<FJsonValueObject>(BuildGraphJson(Graph)));
		}
		Out->SetArrayField(TEXT("graphs"), GraphValues);
		Out->SetNumberField(TEXT("graph_count"), GraphValues.Num());
		Out->SetObjectField(TEXT("variables"), BuildVariablesJson(Blueprint));
		return Out;
	}

	static bool ApplyPreviewJson(UControlRigBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Obj, const FString& Path, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		if (!Blueprint || !Obj.IsValid())
		{
			return true;
		}

		UeAgentJsonDiagnostics::WarnUnknownFields(Obj, Path, { TEXT("schema"), TEXT("preview_skeletal_mesh"), TEXT("import_hierarchy_from_preview") }, Issues);
		FString PreviewMeshPath;
		if (Obj->TryGetStringField(TEXT("preview_skeletal_mesh"), PreviewMeshPath))
		{
			if (PreviewMeshPath.TrimStartAndEnd().IsEmpty())
			{
				Blueprint->SetPreviewMesh(nullptr, true);
			}
			else if (USkeletalMesh* PreviewMesh = LoadAsset<USkeletalMesh>(PreviewMeshPath))
			{
				Blueprint->SetPreviewMesh(PreviewMesh, true);
				bool bImportHierarchy = false;
				if (UeAgentJsonDiagnostics::ReadBoolField(Obj, TEXT("import_hierarchy_from_preview"), Path + TEXT(".import_hierarchy_from_preview"), bImportHierarchy, Issues, false) && bImportHierarchy)
				{
					Blueprint->GetHierarchyController()->ImportBonesFromSkeletalMesh(PreviewMesh, NAME_None, true, true, false, false, false);
				}
			}
			else
			{
				UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("warning"), TEXT("asset_reference_not_found"), Path + TEXT(".preview_skeletal_mesh"), FString::Printf(TEXT("Preview skeletal mesh '%s' could not be loaded."), *PreviewMeshPath));
			}
		}
		return !HasBlockingIssue(Issues);
	}

	static bool ApplyShapeLibrariesJson(UControlRigBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Obj, const FString& Path, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		if (!Blueprint || !Obj.IsValid())
		{
			return true;
		}

		UeAgentJsonDiagnostics::WarnUnknownFields(Obj, Path, { TEXT("schema"), TEXT("shape_libraries"), TEXT("name_map") }, Issues);
		const TArray<TSharedPtr<FJsonValue>>* Libraries = nullptr;
		if (!UeAgentJsonDiagnostics::ReadArrayField(Obj, TEXT("shape_libraries"), Path + TEXT(".shape_libraries"), Libraries, Issues, false) || !Libraries)
		{
			return !HasBlockingIssue(Issues);
		}

		Blueprint->ShapeLibraries.Reset();
		for (int32 Index = 0; Index < Libraries->Num(); ++Index)
		{
			FString LibraryPath;
			if (!(*Libraries)[Index]->TryGetString(LibraryPath))
			{
				UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("warning"), TEXT("json_array_item_type_mismatch"), FString::Printf(TEXT("%s.shape_libraries[%d]"), *Path, Index), TEXT("Expected shape library asset path string."));
				continue;
			}
			if (!LibraryPath.TrimStartAndEnd().IsEmpty())
			{
				if (!LoadShapeLibrary(LibraryPath))
				{
					UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("warning"), TEXT("asset_reference_not_found"), FString::Printf(TEXT("%s.shape_libraries[%d]"), *Path, Index), FString::Printf(TEXT("Shape library '%s' could not be loaded."), *LibraryPath));
				}
				Blueprint->ShapeLibraries.Add(TSoftObjectPtr<UControlRigShapeLibrary>(FSoftObjectPath(ToObjectPath(LibraryPath))));
			}
		}
		return !HasBlockingIssue(Issues);
	}

	static void ApplyBonesJson(UControlRigBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Obj, const FString& Path, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		if (!Blueprint || !Obj.IsValid())
		{
			return;
		}
		UeAgentJsonDiagnostics::WarnUnknownFields(Obj, Path, { TEXT("replace_existing"), TEXT("bones") }, Issues);

		URigHierarchy* Hierarchy = Blueprint->GetHierarchy();
		URigHierarchyController* Controller = Blueprint->GetHierarchyController();
		if (!Hierarchy || !Controller)
		{
			UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("control_rig_hierarchy_missing"), Path, TEXT("Control Rig hierarchy/controller is missing."));
			return;
		}

		bool bReplaceExisting = false;
		if (UeAgentJsonDiagnostics::ReadBoolField(Obj, TEXT("replace_existing"), Path + TEXT(".replace_existing"), bReplaceExisting, Issues, false) && bReplaceExisting)
		{
			Hierarchy->Reset();
		}

		const TArray<TSharedPtr<FJsonValue>>* Bones = nullptr;
		if (!UeAgentJsonDiagnostics::ReadArrayField(Obj, TEXT("bones"), Path + TEXT(".bones"), Bones, Issues, false) || !Bones)
		{
			return;
		}

		for (int32 Index = 0; Index < Bones->Num(); ++Index)
		{
			TSharedPtr<FJsonObject> BoneObj;
			const FString BonePath = FString::Printf(TEXT("%s.bones[%d]"), *Path, Index);
			if (!UeAgentJsonDiagnostics::ReadObjectFromValue((*Bones)[Index], BonePath, BoneObj, Issues))
			{
				continue;
			}
			UeAgentJsonDiagnostics::WarnUnknownFields(BoneObj, BonePath, { TEXT("name"), TEXT("parent"), TEXT("parent_name"), TEXT("parent_type"), TEXT("bone_type"), TEXT("initial_local_transform"), TEXT("current_local_transform"), TEXT("remove") }, Issues);

			FString Name;
			if (!UeAgentJsonDiagnostics::ReadStringField(BoneObj, TEXT("name"), BonePath + TEXT(".name"), Name, Issues, true) || Name.TrimStartAndEnd().IsEmpty())
			{
				continue;
			}
			const FRigElementKey Key(FName(*Name), ERigElementType::Bone);
			bool bRemove = false;
			if (UeAgentJsonDiagnostics::ReadBoolField(BoneObj, TEXT("remove"), BonePath + TEXT(".remove"), bRemove, Issues, false) && bRemove)
			{
				Controller->RemoveElement(Key, false, false);
				continue;
			}

			FTransform LocalTransform = FTransform::Identity;
			ReadTransformField(BoneObj, TEXT("initial_local_transform"), BonePath + TEXT(".initial_local_transform"), LocalTransform, Issues, false);
			ERigBoneType BoneType = ERigBoneType::User;
			FString BoneTypeText;
			if (BoneObj->TryGetStringField(TEXT("bone_type"), BoneTypeText) && !ParseEnumString(BoneTypeText, BoneType))
			{
				UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("warning"), TEXT("json_enum_parse_failed"), BonePath + TEXT(".bone_type"), FString::Printf(TEXT("Unknown ERigBoneType '%s'."), *BoneTypeText));
			}

			if (!Hierarchy->Find(Key))
			{
				Controller->AddBone(FName(*Name), ReadParentKey(BoneObj, ERigElementType::Bone), LocalTransform, false, BoneType, false, false);
			}
			else
			{
				Hierarchy->SetLocalTransform(Key, LocalTransform, true, true, false, false);
				const FRigElementKey ParentKey = ReadParentKey(BoneObj, ERigElementType::Bone);
				if (ParentKey.IsValid())
				{
					Controller->SetParent(Key, ParentKey, false, false, false);
				}
			}
		}
	}

	static void ApplyNullsJson(UControlRigBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Obj, const FString& Path, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		if (!Blueprint || !Obj.IsValid())
		{
			return;
		}
		UeAgentJsonDiagnostics::WarnUnknownFields(Obj, Path, { TEXT("nulls") }, Issues);

		URigHierarchy* Hierarchy = Blueprint->GetHierarchy();
		URigHierarchyController* Controller = Blueprint->GetHierarchyController();
		const TArray<TSharedPtr<FJsonValue>>* Nulls = nullptr;
		if (!Hierarchy || !Controller || !UeAgentJsonDiagnostics::ReadArrayField(Obj, TEXT("nulls"), Path + TEXT(".nulls"), Nulls, Issues, false) || !Nulls)
		{
			return;
		}

		for (int32 Index = 0; Index < Nulls->Num(); ++Index)
		{
			TSharedPtr<FJsonObject> NullObj;
			const FString NullPath = FString::Printf(TEXT("%s.nulls[%d]"), *Path, Index);
			if (!UeAgentJsonDiagnostics::ReadObjectFromValue((*Nulls)[Index], NullPath, NullObj, Issues))
			{
				continue;
			}
			UeAgentJsonDiagnostics::WarnUnknownFields(NullObj, NullPath, { TEXT("name"), TEXT("parent"), TEXT("parent_name"), TEXT("parent_type"), TEXT("initial_local_transform"), TEXT("current_local_transform"), TEXT("remove") }, Issues);

			FString Name;
			if (!UeAgentJsonDiagnostics::ReadStringField(NullObj, TEXT("name"), NullPath + TEXT(".name"), Name, Issues, true) || Name.TrimStartAndEnd().IsEmpty())
			{
				continue;
			}
			const FRigElementKey Key(FName(*Name), ERigElementType::Null);
			bool bRemove = false;
			if (UeAgentJsonDiagnostics::ReadBoolField(NullObj, TEXT("remove"), NullPath + TEXT(".remove"), bRemove, Issues, false) && bRemove)
			{
				Controller->RemoveElement(Key, false, false);
				continue;
			}

			FTransform LocalTransform = FTransform::Identity;
			ReadTransformField(NullObj, TEXT("initial_local_transform"), NullPath + TEXT(".initial_local_transform"), LocalTransform, Issues, false);
			if (!Hierarchy->Find(Key))
			{
				Controller->AddNull(FName(*Name), ReadParentKey(NullObj, ERigElementType::Bone), LocalTransform, false, false, false);
			}
			else
			{
				Hierarchy->SetLocalTransform(Key, LocalTransform, true, true, false, false);
				const FRigElementKey ParentKey = ReadParentKey(NullObj, ERigElementType::Bone);
				if (ParentKey.IsValid())
				{
					Controller->SetParent(Key, ParentKey, false, false, false);
				}
			}
		}
	}

	static void ApplyControlsJson(UControlRigBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Obj, const FString& Path, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		if (!Blueprint || !Obj.IsValid())
		{
			return;
		}
		UeAgentJsonDiagnostics::WarnUnknownFields(Obj, Path, { TEXT("controls") }, Issues);

		URigHierarchy* Hierarchy = Blueprint->GetHierarchy();
		URigHierarchyController* Controller = Blueprint->GetHierarchyController();
		const TArray<TSharedPtr<FJsonValue>>* Controls = nullptr;
		if (!Hierarchy || !Controller || !UeAgentJsonDiagnostics::ReadArrayField(Obj, TEXT("controls"), Path + TEXT(".controls"), Controls, Issues, false) || !Controls)
		{
			return;
		}

		for (int32 Index = 0; Index < Controls->Num(); ++Index)
		{
			TSharedPtr<FJsonObject> ControlObj;
			const FString ControlPath = FString::Printf(TEXT("%s.controls[%d]"), *Path, Index);
			if (!UeAgentJsonDiagnostics::ReadObjectFromValue((*Controls)[Index], ControlPath, ControlObj, Issues))
			{
				continue;
			}
			UeAgentJsonDiagnostics::WarnUnknownFields(ControlObj, ControlPath, { TEXT("name"), TEXT("parent"), TEXT("parent_name"), TEXT("parent_type"), TEXT("settings"), TEXT("value_transform"), TEXT("offset_transform"), TEXT("shape_transform"), TEXT("remove") }, Issues);

			FString Name;
			if (!UeAgentJsonDiagnostics::ReadStringField(ControlObj, TEXT("name"), ControlPath + TEXT(".name"), Name, Issues, true) || Name.TrimStartAndEnd().IsEmpty())
			{
				continue;
			}
			const FRigElementKey Key(FName(*Name), ERigElementType::Control);
			bool bRemove = false;
			if (UeAgentJsonDiagnostics::ReadBoolField(ControlObj, TEXT("remove"), ControlPath + TEXT(".remove"), bRemove, Issues, false) && bRemove)
			{
				Controller->RemoveElement(Key, false, false);
				continue;
			}

			FRigControlSettings Settings;
			if (FRigControlElement* ExistingControl = Hierarchy->Find<FRigControlElement>(Key))
			{
				Settings = ExistingControl->Settings;
			}
			const TSharedPtr<FJsonObject>* SettingsObj = nullptr;
			if (ControlObj->TryGetObjectField(TEXT("settings"), SettingsObj) && SettingsObj && SettingsObj->IsValid())
			{
				ApplyControlSettingsJson(*SettingsObj, ControlPath + TEXT(".settings"), Settings, Issues);
			}
			Settings.SetupLimitArrayForType();

			FTransform ValueTransform = FTransform::Identity;
			ReadTransformField(ControlObj, TEXT("value_transform"), ControlPath + TEXT(".value_transform"), ValueTransform, Issues, false);
			FRigControlValue ControlValue;
			ControlValue.SetFromTransform(ValueTransform, Settings.ControlType, Settings.PrimaryAxis);

			FTransform OffsetTransform = FTransform::Identity;
			ReadTransformField(ControlObj, TEXT("offset_transform"), ControlPath + TEXT(".offset_transform"), OffsetTransform, Issues, false);
			FTransform ShapeTransform = FTransform::Identity;
			ReadTransformField(ControlObj, TEXT("shape_transform"), ControlPath + TEXT(".shape_transform"), ShapeTransform, Issues, false);

			if (!Hierarchy->Find(Key))
			{
				Controller->AddControl(FName(*Name), ReadParentKey(ControlObj, ERigElementType::Bone), Settings, ControlValue, OffsetTransform, ShapeTransform, false, false);
			}
			else
			{
				Controller->SetControlSettings(Key, Settings, false);
				Hierarchy->SetControlValue(Key, ControlValue, ERigControlValueType::Initial, false, false);
				Hierarchy->SetControlValue(Key, ControlValue, ERigControlValueType::Current, false, false);
				Hierarchy->SetControlOffsetTransform(Key, OffsetTransform, true, true, false, false);
				Hierarchy->SetControlShapeTransform(Key, ShapeTransform, true, false);
				const FRigElementKey ParentKey = ReadParentKey(ControlObj, ERigElementType::Bone);
				if (ParentKey.IsValid())
				{
					Controller->SetParent(Key, ParentKey, false, false, false);
				}
			}
		}
	}

	static void ApplyCurvesJson(UControlRigBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Obj, const FString& Path, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		if (!Blueprint || !Obj.IsValid())
		{
			return;
		}
		UeAgentJsonDiagnostics::WarnUnknownFields(Obj, Path, { TEXT("curves") }, Issues);

		URigHierarchy* Hierarchy = Blueprint->GetHierarchy();
		URigHierarchyController* Controller = Blueprint->GetHierarchyController();
		const TArray<TSharedPtr<FJsonValue>>* Curves = nullptr;
		if (!Hierarchy || !Controller || !UeAgentJsonDiagnostics::ReadArrayField(Obj, TEXT("curves"), Path + TEXT(".curves"), Curves, Issues, false) || !Curves)
		{
			return;
		}

		for (int32 Index = 0; Index < Curves->Num(); ++Index)
		{
			TSharedPtr<FJsonObject> CurveObj;
			const FString CurvePath = FString::Printf(TEXT("%s.curves[%d]"), *Path, Index);
			if (!UeAgentJsonDiagnostics::ReadObjectFromValue((*Curves)[Index], CurvePath, CurveObj, Issues))
			{
				continue;
			}
			UeAgentJsonDiagnostics::WarnUnknownFields(CurveObj, CurvePath, { TEXT("name"), TEXT("value"), TEXT("remove") }, Issues);

			FString Name;
			if (!UeAgentJsonDiagnostics::ReadStringField(CurveObj, TEXT("name"), CurvePath + TEXT(".name"), Name, Issues, true) || Name.TrimStartAndEnd().IsEmpty())
			{
				continue;
			}
			const FRigElementKey Key(FName(*Name), ERigElementType::Curve);
			bool bRemove = false;
			if (UeAgentJsonDiagnostics::ReadBoolField(CurveObj, TEXT("remove"), CurvePath + TEXT(".remove"), bRemove, Issues, false) && bRemove)
			{
				Controller->RemoveElement(Key, false, false);
				continue;
			}

			double Value = 0.0;
			const TSharedPtr<FJsonValue>* ValueField = UeAgentJsonDiagnostics::FindFieldValue(CurveObj, TEXT("value"));
			if (ValueField && !UeAgentJsonDiagnostics::TryReadNumberValue(*ValueField, Value))
			{
				UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("warning"), TEXT("json_field_type_mismatch"), CurvePath + TEXT(".value"), FString::Printf(TEXT("Expected number but got %s."), *UeAgentJsonDiagnostics::JsonValueTypeToString(*ValueField)));
			}
			if (!Hierarchy->Find(Key))
			{
				Controller->AddCurve(FName(*Name), static_cast<float>(Value), false, false);
			}
			else
			{
				Hierarchy->SetCurveValue(Key, static_cast<float>(Value), false);
			}
		}
	}

	static bool ApplyGraphsJson(UControlRigBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Obj, const FString& Path, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		if (!Blueprint || !Obj.IsValid())
		{
			return true;
		}
		UeAgentJsonDiagnostics::WarnUnknownFields(Obj, Path, { TEXT("replace_nodes"), TEXT("graphs") }, Issues);

		const TArray<TSharedPtr<FJsonValue>>* Graphs = nullptr;
		if (!UeAgentJsonDiagnostics::ReadArrayField(Obj, TEXT("graphs"), Path + TEXT(".graphs"), Graphs, Issues, false) || !Graphs)
		{
			return !HasBlockingIssue(Issues);
		}

		bool bReplaceNodes = false;
		UeAgentJsonDiagnostics::ReadBoolField(Obj, TEXT("replace_nodes"), Path + TEXT(".replace_nodes"), bReplaceNodes, Issues, false);

		for (int32 GraphIndex = 0; GraphIndex < Graphs->Num(); ++GraphIndex)
		{
			TSharedPtr<FJsonObject> GraphObj;
			const FString GraphPath = FString::Printf(TEXT("%s.graphs[%d]"), *Path, GraphIndex);
			if (!UeAgentJsonDiagnostics::ReadObjectFromValue((*Graphs)[GraphIndex], GraphPath, GraphObj, Issues))
			{
				continue;
			}
			UeAgentJsonDiagnostics::WarnUnknownFields(GraphObj, GraphPath, { TEXT("name"), TEXT("default_graph"), TEXT("nodes"), TEXT("links") }, Issues);

			FString GraphName;
			GraphObj->TryGetStringField(TEXT("name"), GraphName);
			URigVMGraph* Graph = GraphName.TrimStartAndEnd().IsEmpty() ? Blueprint->GetDefaultModel() : Blueprint->GetModel(GraphName);
			if (!Graph)
			{
				Graph = Blueprint->AddModel(GraphName.TrimStartAndEnd().IsEmpty() ? TEXT("Rig Graph") : GraphName, true, false);
			}
			URigVMController* Controller = Blueprint->GetController(Graph);
			if (!Graph || !Controller)
			{
				UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("control_rig_graph_controller_missing"), GraphPath, TEXT("RigVM graph/controller is missing."));
				continue;
			}

			if (bReplaceNodes)
			{
				TArray<URigVMNode*> ExistingNodes = Graph->GetNodes();
				Controller->RemoveNodes(ExistingNodes, false, false);
			}

			const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
			if (UeAgentJsonDiagnostics::ReadArrayField(GraphObj, TEXT("nodes"), GraphPath + TEXT(".nodes"), Nodes, Issues, false) && Nodes)
			{
				for (int32 NodeIndex = 0; NodeIndex < Nodes->Num(); ++NodeIndex)
				{
					TSharedPtr<FJsonObject> NodeObj;
					const FString NodePath = FString::Printf(TEXT("%s.nodes[%d]"), *GraphPath, NodeIndex);
					if (!UeAgentJsonDiagnostics::ReadObjectFromValue((*Nodes)[NodeIndex], NodePath, NodeObj, Issues))
					{
						continue;
					}
					UeAgentJsonDiagnostics::WarnUnknownFields(NodeObj, NodePath, { TEXT("node_type"), TEXT("name"), TEXT("title"), TEXT("position"), TEXT("script_struct"), TEXT("method"), TEXT("notation"), TEXT("variable_name"), TEXT("cpp_type"), TEXT("cpp_type_object_path"), TEXT("is_getter"), TEXT("default_value"), TEXT("pin_defaults") }, Issues);

					FString NodeType;
					FString NodeName;
					NodeObj->TryGetStringField(TEXT("node_type"), NodeType);
					NodeObj->TryGetStringField(TEXT("name"), NodeName);
					FVector2D Position = FVector2D::ZeroVector;
					ReadVector2DField(NodeObj, TEXT("position"), NodePath + TEXT(".position"), Position, Issues, false);

					URigVMNode* NewNode = nullptr;
					if (NodeType.Equals(TEXT("unit"), ESearchCase::IgnoreCase))
					{
						FString ScriptStructPath;
						if (UeAgentJsonDiagnostics::ReadStringField(NodeObj, TEXT("script_struct"), NodePath + TEXT(".script_struct"), ScriptStructPath, Issues, true))
						{
							FString MethodText = TEXT("Execute");
							NodeObj->TryGetStringField(TEXT("method"), MethodText);
							NewNode = Controller->AddUnitNodeFromStructPath(ScriptStructPath, FName(*MethodText), Position, NodeName, true, false);
							if (!NewNode)
							{
								UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("warning"), TEXT("rigvm_add_unit_node_failed"), NodePath, FString::Printf(TEXT("Failed to add unit node for struct '%s'."), *ScriptStructPath));
							}
						}
					}
					else if (NodeType.Equals(TEXT("template"), ESearchCase::IgnoreCase))
					{
						FString Notation;
						if (UeAgentJsonDiagnostics::ReadStringField(NodeObj, TEXT("notation"), NodePath + TEXT(".notation"), Notation, Issues, true))
						{
							NewNode = Controller->AddTemplateNode(FName(*Notation), Position, NodeName, true, false);
							if (!NewNode)
							{
								UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("warning"), TEXT("rigvm_add_template_node_failed"), NodePath, FString::Printf(TEXT("Failed to add template node '%s'."), *Notation));
							}
						}
					}
					else if (NodeType.Equals(TEXT("variable"), ESearchCase::IgnoreCase))
					{
						FString VariableName;
						FString CppType;
						FString CppTypeObjectPath;
						FString DefaultValue;
						bool bIsGetter = true;
						UeAgentJsonDiagnostics::ReadStringField(NodeObj, TEXT("variable_name"), NodePath + TEXT(".variable_name"), VariableName, Issues, true);
						UeAgentJsonDiagnostics::ReadStringField(NodeObj, TEXT("cpp_type"), NodePath + TEXT(".cpp_type"), CppType, Issues, true);
						NodeObj->TryGetStringField(TEXT("cpp_type_object_path"), CppTypeObjectPath);
						NodeObj->TryGetStringField(TEXT("default_value"), DefaultValue);
						UeAgentJsonDiagnostics::ReadBoolField(NodeObj, TEXT("is_getter"), NodePath + TEXT(".is_getter"), bIsGetter, Issues, false);
						if (!VariableName.IsEmpty() && !CppType.IsEmpty())
						{
							NewNode = Controller->AddVariableNodeFromObjectPath(FName(*VariableName), CppType, CppTypeObjectPath, bIsGetter, DefaultValue, Position, NodeName, true, false);
							if (!NewNode)
							{
								UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("warning"), TEXT("rigvm_add_variable_node_failed"), NodePath, FString::Printf(TEXT("Failed to add variable node '%s'."), *VariableName));
							}
						}
					}

					const TSharedPtr<FJsonObject>* PinDefaultsObj = nullptr;
					if (NewNode && NodeObj->TryGetObjectField(TEXT("pin_defaults"), PinDefaultsObj) && PinDefaultsObj && PinDefaultsObj->IsValid())
					{
						for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*PinDefaultsObj)->Values)
						{
							FString ValueText;
							if (!Pair.Value.IsValid() || !Pair.Value->TryGetString(ValueText))
							{
								UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("warning"), TEXT("json_field_type_mismatch"), NodePath + TEXT(".pin_defaults.") + Pair.Key, TEXT("Pin default values must be strings."));
								continue;
							}
							const FString FullPinPath = Pair.Key.StartsWith(NewNode->GetName() + TEXT(".")) ? Pair.Key : NewNode->GetName() + TEXT(".") + Pair.Key;
							if (!Controller->SetPinDefaultValue(FullPinPath, ValueText, true, true, false, false, true))
							{
								UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("warning"), TEXT("rigvm_set_pin_default_failed"), NodePath + TEXT(".pin_defaults.") + Pair.Key, FString::Printf(TEXT("Failed to set pin default '%s'."), *FullPinPath));
							}
						}
					}
				}
			}

			const TArray<TSharedPtr<FJsonValue>>* Links = nullptr;
			if (UeAgentJsonDiagnostics::ReadArrayField(GraphObj, TEXT("links"), GraphPath + TEXT(".links"), Links, Issues, false) && Links)
			{
				for (int32 LinkIndex = 0; LinkIndex < Links->Num(); ++LinkIndex)
				{
					TSharedPtr<FJsonObject> LinkObj;
					const FString LinkPath = FString::Printf(TEXT("%s.links[%d]"), *GraphPath, LinkIndex);
					if (!UeAgentJsonDiagnostics::ReadObjectFromValue((*Links)[LinkIndex], LinkPath, LinkObj, Issues))
					{
						continue;
					}
					UeAgentJsonDiagnostics::WarnUnknownFields(LinkObj, LinkPath, { TEXT("source"), TEXT("target") }, Issues);

					FString Source;
					FString Target;
					UeAgentJsonDiagnostics::ReadStringField(LinkObj, TEXT("source"), LinkPath + TEXT(".source"), Source, Issues, true);
					UeAgentJsonDiagnostics::ReadStringField(LinkObj, TEXT("target"), LinkPath + TEXT(".target"), Target, Issues, true);
					if (!Source.IsEmpty() && !Target.IsEmpty() && !Controller->AddLink(Source, Target, true, false))
					{
						UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("warning"), TEXT("rigvm_add_link_failed"), LinkPath, FString::Printf(TEXT("Failed to link '%s' -> '%s'."), *Source, *Target));
					}
				}
			}
		}
		return !HasBlockingIssue(Issues);
	}

	static void MarkControlRigModified(UControlRigBlueprint* Blueprint)
	{
		if (!Blueprint)
		{
			return;
		}
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		Blueprint->MarkPackageDirty();
	}

	static TArray<TSharedPtr<FJsonValue>> MakeStringArray(std::initializer_list<const TCHAR*> Items)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const TCHAR* Item : Items)
		{
			Values.Add(MakeShared<FJsonValueString>(FString(Item)));
		}
		return Values;
	}

	static TSharedPtr<FJsonObject> MakeEmptyItemsJson(const FString& Schema, const FString& FieldName = TEXT("items"))
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!Schema.IsEmpty())
		{
			Obj->SetStringField(TEXT("schema"), Schema);
		}
		Obj->SetArrayField(FieldName, {});
		return Obj;
	}

	static TSharedPtr<FJsonObject> MakeUnsupportedPlaceholderJson(const FString& Schema, const FString& Profile, const FString& ApplyPolicy)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("schema"), Schema);
		Obj->SetStringField(TEXT("profile"), Profile);
		Obj->SetStringField(TEXT("apply_policy"), ApplyPolicy);
		Obj->SetArrayField(TEXT("items"), {});
		return Obj;
	}

	static TSharedPtr<FJsonObject> MakeCompileReportJson(UControlRigBlueprint* Blueprint, const FCompilerResultsLog* CompilerLog = nullptr)
	{
		TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
		Report->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.control_rig.compile_report.v1"));
		Report->SetStringField(TEXT("asset_path"), Blueprint ? NormalizeAssetPath(Blueprint->GetPathName()) : FString());
		Report->SetNumberField(TEXT("blueprint_status"), Blueprint ? static_cast<int32>(Blueprint->Status) : 0);
		Report->SetBoolField(TEXT("compiled"), Blueprint ? Blueprint->Status != BS_Error : false);

		TArray<TSharedPtr<FJsonValue>> Messages;
		int32 ErrorCount = 0;
		int32 WarningCount = 0;
		if (CompilerLog)
		{
			ErrorCount = CompilerLog->NumErrors;
			WarningCount = CompilerLog->NumWarnings;
			for (const TSharedRef<FTokenizedMessage>& Message : CompilerLog->Messages)
			{
				TSharedPtr<FJsonObject> MessageObj = MakeShared<FJsonObject>();
				FString Severity = TEXT("unknown");
				switch (Message->GetSeverity())
				{
				case EMessageSeverity::Error: Severity = TEXT("error"); break;
				case EMessageSeverity::Warning: Severity = TEXT("warning"); break;
				case EMessageSeverity::PerformanceWarning: Severity = TEXT("performance_warning"); break;
				case EMessageSeverity::Info: Severity = TEXT("info"); break;
				default: break;
				}
				MessageObj->SetStringField(TEXT("severity"), Severity);
				MessageObj->SetStringField(TEXT("message"), Message->ToText().ToString());
				MessageObj->SetStringField(TEXT("identifier"), Message->GetIdentifier().ToString());
				Messages.Add(MakeShared<FJsonValueObject>(MessageObj));
			}
		}
		Report->SetNumberField(TEXT("error_count"), ErrorCount);
		Report->SetNumberField(TEXT("warning_count"), WarningCount);
		Report->SetNumberField(TEXT("message_count"), Messages.Num());
		Report->SetArrayField(TEXT("messages"), Messages);
		return Report;
	}

	static TSharedPtr<FJsonObject> MakeRuntimeProbeReportJson(const FString& Mode = TEXT("not_run"))
	{
		TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
		Report->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.control_rig.runtime_probe_report.v1"));
		Report->SetStringField(TEXT("mode"), Mode);
		Report->SetBoolField(TEXT("executed"), false);
		Report->SetArrayField(TEXT("issues"), {});
		return Report;
	}

	static TSharedPtr<FJsonObject> MakeRigVMUnitRegistryJson()
	{
		TSharedPtr<FJsonObject> Registry = MakeShared<FJsonObject>();
		Registry->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.control_rig.unit_registry.v1"));
		Registry->SetStringField(TEXT("engine_target"), TEXT("UE_5.6"));

		TArray<TSharedPtr<FJsonValue>> UnitValues;
		for (TObjectIterator<UScriptStruct> It; It; ++It)
		{
			UScriptStruct* Struct = *It;
			if (!Struct || Struct == FRigUnit::StaticStruct() || !Struct->IsChildOf(FRigUnit::StaticStruct()))
			{
				continue;
			}

			TSharedPtr<FJsonObject> UnitObj = MakeShared<FJsonObject>();
			UnitObj->SetStringField(TEXT("struct_path"), Struct->GetPathName());
			UnitObj->SetStringField(TEXT("struct_name"), Struct->GetName());
			UnitObj->SetStringField(TEXT("display_name"), Struct->GetDisplayNameText().ToString());
			UnitObj->SetStringField(TEXT("category"), Struct->GetMetaData(TEXT("Category")));
			UnitObj->SetBoolField(TEXT("deprecated"), Struct->HasMetaData(TEXT("Deprecated")));

			TArray<TSharedPtr<FJsonValue>> PinValues;
			for (TFieldIterator<FProperty> PropIt(Struct); PropIt; ++PropIt)
			{
				const FProperty* Prop = *PropIt;
				if (!Prop)
				{
					continue;
				}

				TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
				FString ExtendedType;
				PinObj->SetStringField(TEXT("name"), Prop->GetName());
				PinObj->SetStringField(TEXT("cpp_type"), Prop->GetCPPType(&ExtendedType) + ExtendedType);
				if (Prop->HasMetaData(TEXT("Input")))
				{
					PinObj->SetStringField(TEXT("direction"), TEXT("input"));
				}
				else if (Prop->HasMetaData(TEXT("Output")))
				{
					PinObj->SetStringField(TEXT("direction"), TEXT("output"));
				}
				else
				{
					PinObj->SetStringField(TEXT("direction"), TEXT("value"));
				}
				PinValues.Add(MakeShared<FJsonValueObject>(PinObj));
			}
			UnitObj->SetArrayField(TEXT("pins"), PinValues);
			UnitValues.Add(MakeShared<FJsonValueObject>(UnitObj));
		}

		Registry->SetArrayField(TEXT("units"), UnitValues);
		Registry->SetNumberField(TEXT("unit_count"), UnitValues.Num());
		return Registry;
	}

	static bool HasUnsupportedWriteContent(const TSharedPtr<FJsonObject>& Obj)
	{
		if (!Obj.IsValid())
		{
			return false;
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Obj->Values)
		{
			if (Pair.Key == TEXT("schema") || Pair.Key == TEXT("profile") || Pair.Key == TEXT("apply_policy") || Pair.Key == TEXT("notes"))
			{
				continue;
			}
			if (!Pair.Value.IsValid() || Pair.Value->IsNull())
			{
				continue;
			}
			if (Pair.Value->Type == EJson::Array)
			{
				const TArray<TSharedPtr<FJsonValue>>* ArrayValue = nullptr;
				if (Pair.Value->TryGetArray(ArrayValue) && ArrayValue && ArrayValue->Num() == 0)
				{
					continue;
				}
				return true;
			}
			if (Pair.Value->Type == EJson::Object)
			{
				const TSharedPtr<FJsonObject>* ObjectValue = nullptr;
				if (Pair.Value->TryGetObject(ObjectValue) && ObjectValue && ObjectValue->IsValid() && (*ObjectValue)->Values.Num() == 0)
				{
					continue;
				}
				return true;
			}
			FString StringValue;
			if (Pair.Value->TryGetString(StringValue) && StringValue.IsEmpty())
			{
				continue;
			}
			return true;
		}
		return false;
	}

	static void AddUnsupportedApplyIssueIfNeeded(
		const TSharedPtr<FJsonObject>& Obj,
		const FString& FilePath,
		const FString& Profile,
		TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		if (!HasUnsupportedWriteContent(Obj))
		{
			return;
		}
		UeAgentJsonDiagnostics::AddIssue(
			Issues,
			TEXT("error"),
			TEXT("unsupported_apply_profile"),
			FilePath,
			FString::Printf(TEXT("%s contains requested writes, but this profile is not applied by control_rig_apply_folder. Move supported state into the documented hierarchy/graphs/settings files or use the explicit action workflow."), *Profile));
	}

	static TSharedPtr<FJsonObject> MakeCoverageJson()
	{
		TSharedPtr<FJsonObject> Coverage = MakeShared<FJsonObject>();
		Coverage->SetStringField(TEXT("status"), TEXT("complete_folder_profile"));
		Coverage->SetStringField(TEXT("implementation_status"), TEXT("complete_folder_profile"));
		Coverage->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.control_rig.folder.v1"));
		Coverage->SetBoolField(TEXT("is_complete_target_schema"), true);
		Coverage->SetArrayField(TEXT("covered_profiles"), MakeStringArray({
			TEXT("asset"),
			TEXT("settings_preview"),
			TEXT("shape_library_reference"),
			TEXT("hierarchy_bones"),
			TEXT("hierarchy_nulls"),
			TEXT("hierarchy_controls"),
			TEXT("hierarchy_curves"),
			TEXT("variables"),
			TEXT("rigvm_graphs"),
			TEXT("compile_report"),
			TEXT("runtime_probe"),
			TEXT("editor_view"),
			TEXT("shape_library_asset")
		}));
		Coverage->SetArrayField(TEXT("readonly_profiles"), MakeStringArray({
			TEXT("generated_class"),
			TEXT("compiled_vm_bytecode"),
			TEXT("editor_session_only_view_state")
		}));
		Coverage->SetArrayField(TEXT("unsupported_apply"), MakeStringArray({
			TEXT("variables/exposed_properties.json writes"),
			TEXT("functions/functions.json writes"),
			TEXT("modular/modules.json writes"),
			TEXT("modular/connections.json writes"),
			TEXT("modular/connectors.json writes"),
			TEXT("raw_properties.json fallback writes"),
			TEXT("readonly_properties.json writes")
		}));
		Coverage->SetNumberField(TEXT("unsupported_apply_count"), 7);
		Coverage->SetArrayField(TEXT("blocking_gaps"), {});
		Coverage->SetStringField(TEXT("covered"), TEXT("asset settings, preview mesh, shape library references, hierarchy bones/nulls/controls/curves, RigVM graph nodes/links/pin defaults, compile report, runtime probe, editor view commands"));
		Coverage->SetStringField(TEXT("notes"), TEXT("Control Rig asset authoring is folder JSON first. Cross-asset AnimBlueprint/Sequencer integration stays in those asset workflows or explicit action commands."));
		return Coverage;
	}
}

bool FUeAgentHttpServer::ExecuteControlRigShapeLibraryCommand(const FString& CommandLower, const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (CommandLower == TEXT("control_rig_shape_library_create"))
	{
		return CmdControlRigShapeLibraryCreate(Ctx, OutData, OutError);
	}
	if (CommandLower == TEXT("control_rig_shape_library_get_info"))
	{
		return CmdControlRigShapeLibraryGetInfo(Ctx, OutData, OutError);
	}
	if (CommandLower == TEXT("control_rig_shape_library_export_json"))
	{
		return CmdControlRigShapeLibraryExportJson(Ctx, OutData, OutError);
	}
	if (CommandLower == TEXT("control_rig_shape_library_validate_json"))
	{
		return CmdControlRigShapeLibraryValidateJson(Ctx, OutData, OutError);
	}
	if (CommandLower == TEXT("control_rig_shape_library_apply_json"))
	{
		return CmdControlRigShapeLibraryApplyJson(Ctx, OutData, OutError);
	}
	OutError = TEXT("unknown_control_rig_shape_library_command");
	return false;
}

bool FUeAgentHttpServer::ExecuteControlRigCommand(const FString& CommandLower, const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (CommandLower == TEXT("control_rig_create"))
	{
		return CmdControlRigCreate(Ctx, OutData, OutError);
	}
	if (CommandLower == TEXT("control_rig_get_info"))
	{
		return CmdControlRigGetInfo(Ctx, OutData, OutError);
	}
	if (CommandLower == TEXT("control_rig_export_folder"))
	{
		return CmdControlRigExportFolder(Ctx, OutData, OutError);
	}
	if (CommandLower == TEXT("control_rig_validate_folder"))
	{
		return CmdControlRigValidateFolder(Ctx, OutData, OutError);
	}
	if (CommandLower == TEXT("control_rig_apply_folder"))
	{
		return CmdControlRigApplyFolder(Ctx, OutData, OutError);
	}
	if (CommandLower == TEXT("control_rig_compile"))
	{
		return CmdControlRigCompile(Ctx, OutData, OutError);
	}
	if (CommandLower == TEXT("control_rig_get_compile_log"))
	{
		return CmdControlRigGetCompileLog(Ctx, OutData, OutError);
	}
	if (CommandLower == TEXT("control_rig_open_editor"))
	{
		return CmdControlRigOpenEditor(Ctx, OutData, OutError);
	}
	if (CommandLower == TEXT("control_rig_graph_get_view"))
	{
		return CmdControlRigGraphGetView(Ctx, OutData, OutError);
	}
	if (CommandLower == TEXT("control_rig_graph_set_view"))
	{
		return CmdControlRigGraphSetView(Ctx, OutData, OutError);
	}
	if (CommandLower == TEXT("control_rig_viewport_get_camera"))
	{
		return CmdControlRigViewportGetCamera(Ctx, OutData, OutError);
	}
	if (CommandLower == TEXT("control_rig_viewport_set_camera"))
	{
		return CmdControlRigViewportSetCamera(Ctx, OutData, OutError);
	}
	if (CommandLower == TEXT("control_rig_screenshot"))
	{
		return CmdControlRigScreenshot(Ctx, OutData, OutError);
	}
	if (CommandLower == TEXT("control_rig_runtime_probe"))
	{
		return CmdControlRigRuntimeProbe(Ctx, OutData, OutError);
	}
	if (CommandLower == TEXT("control_rig_bake_to_animation"))
	{
		return CmdControlRigBakeToAnimation(Ctx, OutData, OutError);
	}
	if (CommandLower == TEXT("control_rig_bake_to_control_rig"))
	{
		return CmdControlRigBakeToControlRig(Ctx, OutData, OutError);
	}
	OutError = TEXT("unknown_control_rig_command");
	return false;
}

bool FUeAgentHttpServer::CmdControlRigShapeLibraryCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentHttpServer::CmdControlRigShapeLibraryGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentHttpServer::CmdControlRigShapeLibraryExportJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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
		OutputFile = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetJson"), TEXT("ControlRigShapeLibrary"), SafeName + TEXT(".json"));
	}

	if (!UeAgentControlRigOps::SaveJsonFile(OutputFile, OutData, OutError))
	{
		return false;
	}
	OutData->SetStringField(TEXT("json_file"), UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(OutputFile));
	return true;
}

bool FUeAgentHttpServer::CmdControlRigShapeLibraryValidateJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString JsonFile;
	if (!JsonTryGetString(Ctx.Params, TEXT("json_file"), JsonFile) || JsonFile.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("json_file_required");
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> Issues;
	TSharedPtr<FJsonObject> Root;
	UeAgentControlRigOps::LoadJsonFile(JsonFile, Root, Issues, OutError, false);
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

bool FUeAgentHttpServer::CmdControlRigShapeLibraryApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString JsonFile;
	if (!JsonTryGetString(Ctx.Params, TEXT("json_file"), JsonFile) || JsonFile.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("json_file_required");
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> Issues;
	TSharedPtr<FJsonObject> Root;
	if (!UeAgentControlRigOps::LoadJsonFile(JsonFile, Root, Issues, OutError, false) || !Root.IsValid())
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
		Library->DefaultMaterial = MaterialPath.TrimStartAndEnd().IsEmpty() ? nullptr : UeAgentControlRigOps::LoadAsset<UMaterial>(MaterialPath);
		if (!MaterialPath.TrimStartAndEnd().IsEmpty() && Library->DefaultMaterial.IsNull())
		{
			UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("warning"), TEXT("asset_reference_not_found"), TEXT("shape_library.default_material"), FString::Printf(TEXT("Material '%s' could not be loaded."), *MaterialPath));
		}
	}
	if (Root->TryGetStringField(TEXT("xray_material"), MaterialPath))
	{
		Library->XRayMaterial = MaterialPath.TrimStartAndEnd().IsEmpty() ? nullptr : UeAgentControlRigOps::LoadAsset<UMaterial>(MaterialPath);
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

bool FUeAgentHttpServer::CmdControlRigCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("asset_path_required");
		return false;
	}

	FString RigTypeText;
	JsonTryGetString(Ctx.Params, TEXT("control_rig_type"), RigTypeText);
	EControlRigType RigType = EControlRigType::IndependentRig;
	if (!RigTypeText.TrimStartAndEnd().IsEmpty() && !UeAgentControlRigOps::ParseControlRigTypeText(RigTypeText, RigType))
	{
		OutError = TEXT("invalid_control_rig_type");
		return false;
	}

	FString PreviewMeshPath;
	JsonTryGetString(Ctx.Params, TEXT("preview_skeletal_mesh"), PreviewMeshPath);
	bool bImportHierarchy = true;
	JsonTryGetBool(Ctx.Params, TEXT("import_hierarchy_from_preview"), bImportHierarchy);

	UControlRigBlueprint* Blueprint = UeAgentControlRigOps::CreateControlRigAsset(AssetPath, RigType == EControlRigType::ModularRig, PreviewMeshPath, bImportHierarchy, OutError);
	if (!Blueprint)
	{
		return false;
	}
	if (RigType == EControlRigType::RigModule)
	{
		FString ConvertError;
		if (!Blueprint->TurnIntoControlRigModule(false, &ConvertError))
		{
			OutError = ConvertError.IsEmpty() ? TEXT("turn_into_control_rig_module_failed") : ConvertError;
			return false;
		}
	}

	bool bSaveAfterCreate = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_create"), bSaveAfterCreate);
	if (bSaveAfterCreate && !UeAgentControlRigOps::SaveAssetPackage(Blueprint, OutError))
	{
		return false;
	}

	OutData = UeAgentControlRigOps::BuildControlRigInfo(Blueprint);
	OutData->SetBoolField(TEXT("created"), true);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterCreate);
	return true;
}

bool FUeAgentHttpServer::CmdControlRigGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("asset_path_required");
		return false;
	}

	UControlRigBlueprint* Blueprint = UeAgentControlRigOps::LoadControlRigBlueprint(AssetPath);
	if (!Blueprint)
	{
		OutError = TEXT("control_rig_not_found");
		return false;
	}

	OutData = UeAgentControlRigOps::BuildControlRigInfo(Blueprint);
	return true;
}

bool FUeAgentHttpServer::CmdControlRigExportFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("asset_path_required");
		return false;
	}

	UControlRigBlueprint* Blueprint = UeAgentControlRigOps::LoadControlRigBlueprint(AssetPath);
	if (!Blueprint)
	{
		OutError = TEXT("control_rig_not_found");
		return false;
	}

	FString FolderPath;
	JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderPath);
	if (FolderPath.TrimStartAndEnd().IsEmpty())
	{
		FolderPath = UeAgentControlRigOps::DefaultFolderForAsset(AssetPath);
	}
	FolderPath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(FolderPath);
	IFileManager::Get().MakeDirectory(*FolderPath, true);

	const TSharedPtr<FJsonObject> Info = UeAgentControlRigOps::BuildControlRigInfo(Blueprint);
	TSharedPtr<FJsonObject> AssetJson = MakeShared<FJsonObject>();
	AssetJson->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.control_rig.asset.v1"));
	AssetJson->SetStringField(TEXT("asset_path"), Info->GetStringField(TEXT("asset_path")));
	AssetJson->SetStringField(TEXT("object_path"), Info->GetStringField(TEXT("object_path")));
	AssetJson->SetStringField(TEXT("asset_class"), Info->GetStringField(TEXT("asset_class")));
	AssetJson->SetStringField(TEXT("control_rig_type"), Info->GetStringField(TEXT("control_rig_type")));
	AssetJson->SetStringField(TEXT("control_rig_class"), Info->GetStringField(TEXT("control_rig_class")));
	AssetJson->SetStringField(TEXT("generated_class"), Info->GetStringField(TEXT("generated_class")));
	AssetJson->SetStringField(TEXT("preview_skeletal_mesh"), Info->GetStringField(TEXT("preview_skeletal_mesh")));
	AssetJson->SetNumberField(TEXT("blueprint_status"), Info->GetNumberField(TEXT("blueprint_status")));
	UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("asset.json")), AssetJson, OutError);

	TSharedPtr<FJsonObject> ControlRigSettingsJson = MakeShared<FJsonObject>();
	ControlRigSettingsJson->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.control_rig.settings.v1"));
	ControlRigSettingsJson->SetStringField(TEXT("control_rig_type"), Blueprint->ControlRigType == EControlRigType::RigModule ? TEXT("rig_module") : (Blueprint->ControlRigType == EControlRigType::ModularRig ? TEXT("modular_rig") : TEXT("control_rig")));
	ControlRigSettingsJson->SetStringField(TEXT("apply_policy"), TEXT("read_write_asset_fields_and_readonly_editor_session_fields"));
	ControlRigSettingsJson->SetBoolField(TEXT("auto_recompile"), false);
	ControlRigSettingsJson->SetArrayField(TEXT("event_queue"), UeAgentControlRigOps::MakeStringArray({ TEXT("Construction"), TEXT("Forwards Solve") }));
	UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("settings"), TEXT("control_rig.json")), ControlRigSettingsJson, OutError);

	TSharedPtr<FJsonObject> PreviewJson = MakeShared<FJsonObject>();
	PreviewJson->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.control_rig.preview.v1"));
	PreviewJson->SetStringField(TEXT("preview_skeletal_mesh"), Blueprint->GetPreviewMesh() ? Blueprint->GetPreviewMesh()->GetPathName() : FString());
	PreviewJson->SetBoolField(TEXT("import_hierarchy_from_preview"), false);
	UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("settings"), TEXT("preview.json")), PreviewJson, OutError);

	TSharedPtr<FJsonObject> CompileSettingsJson = MakeShared<FJsonObject>();
	CompileSettingsJson->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.control_rig.vm_compile_settings.v1"));
	CompileSettingsJson->SetStringField(TEXT("apply_policy"), TEXT("readonly_report"));
	CompileSettingsJson->SetBoolField(TEXT("compile_after_apply_default"), true);
	UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("settings"), TEXT("vm_compile_settings.json")), CompileSettingsJson, OutError);

	TSharedPtr<FJsonObject> ShapeRefsJson = MakeShared<FJsonObject>();
	ShapeRefsJson->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.control_rig.shape_library_references.v1"));
	ShapeRefsJson->SetArrayField(TEXT("shape_libraries"), Info->GetArrayField(TEXT("shape_libraries")));
	UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("shape_libraries"), TEXT("references.json")), ShapeRefsJson, OutError);

	const TSharedPtr<FJsonObject>* HierarchyObj = nullptr;
	Info->TryGetObjectField(TEXT("hierarchy"), HierarchyObj);
	if (HierarchyObj && HierarchyObj->IsValid())
	{
		TSharedPtr<FJsonObject> BonesJson = MakeShared<FJsonObject>();
		BonesJson->SetArrayField(TEXT("bones"), (*HierarchyObj)->GetArrayField(TEXT("bones")));
		UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("hierarchy"), TEXT("bones.json")), BonesJson, OutError);

		TSharedPtr<FJsonObject> NullsJson = MakeShared<FJsonObject>();
		NullsJson->SetArrayField(TEXT("nulls"), (*HierarchyObj)->GetArrayField(TEXT("nulls")));
		UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("hierarchy"), TEXT("nulls.json")), NullsJson, OutError);

		TSharedPtr<FJsonObject> ControlsJson = MakeShared<FJsonObject>();
		ControlsJson->SetArrayField(TEXT("controls"), (*HierarchyObj)->GetArrayField(TEXT("controls")));
		UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("hierarchy"), TEXT("controls.json")), ControlsJson, OutError);

		TSharedPtr<FJsonObject> CurvesJson = MakeShared<FJsonObject>();
		CurvesJson->SetArrayField(TEXT("curves"), (*HierarchyObj)->GetArrayField(TEXT("curves")));
		UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("hierarchy"), TEXT("curves.json")), CurvesJson, OutError);
	}

	TSharedPtr<FJsonObject> GraphsJson = MakeShared<FJsonObject>();
	GraphsJson->SetArrayField(TEXT("graphs"), Info->GetArrayField(TEXT("graphs")));
	GraphsJson->SetBoolField(TEXT("replace_nodes"), false);
	UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("graphs"), TEXT("graphs.json")), GraphsJson, OutError);

	UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("graphs"), TEXT("construction_event.json")), UeAgentControlRigOps::MakeUnsupportedPlaceholderJson(TEXT("ue_agent_interface.control_rig.graph.construction_event.v1"), TEXT("construction_event"), TEXT("use graphs/graphs.json for canonical RigVM graph apply")), OutError);
	UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("graphs"), TEXT("forward_solve.json")), UeAgentControlRigOps::MakeUnsupportedPlaceholderJson(TEXT("ue_agent_interface.control_rig.graph.forward_solve.v1"), TEXT("forward_solve"), TEXT("use graphs/graphs.json for canonical RigVM graph apply")), OutError);
	UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("graphs"), TEXT("backward_solve.json")), UeAgentControlRigOps::MakeUnsupportedPlaceholderJson(TEXT("ue_agent_interface.control_rig.graph.backward_solve.v1"), TEXT("backward_solve"), TEXT("use graphs/graphs.json for canonical RigVM graph apply")), OutError);

	UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("hierarchy"), TEXT("connectors.json")), UeAgentControlRigOps::MakeEmptyItemsJson(TEXT("ue_agent_interface.control_rig.hierarchy.connectors.v1")), OutError);
	UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("hierarchy"), TEXT("sockets.json")), UeAgentControlRigOps::MakeEmptyItemsJson(TEXT("ue_agent_interface.control_rig.hierarchy.sockets.v1")), OutError);
	UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("hierarchy"), TEXT("rigid_bodies.json")), UeAgentControlRigOps::MakeEmptyItemsJson(TEXT("ue_agent_interface.control_rig.hierarchy.rigid_bodies.v1")), OutError);
	UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("hierarchy"), TEXT("metadata.json")), UeAgentControlRigOps::MakeEmptyItemsJson(TEXT("ue_agent_interface.control_rig.hierarchy.metadata.v1")), OutError);
	const TSharedPtr<FJsonObject>* VariablesInfoObj = nullptr;
	if (Info->TryGetObjectField(TEXT("variables"), VariablesInfoObj) && VariablesInfoObj && VariablesInfoObj->IsValid())
	{
		UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("variables"), TEXT("variables.json")), *VariablesInfoObj, OutError);
	}
	else
	{
		UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("variables"), TEXT("variables.json")), UeAgentControlRigOps::BuildVariablesJson(Blueprint), OutError);
	}
	UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("variables"), TEXT("exposed_properties.json")), UeAgentControlRigOps::MakeUnsupportedPlaceholderJson(TEXT("ue_agent_interface.control_rig.exposed_properties.v1"), TEXT("exposed_properties"), TEXT("reserved_for_anim_blueprint_mapping profile")), OutError);
	UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("functions"), TEXT("functions.json")), UeAgentControlRigOps::MakeUnsupportedPlaceholderJson(TEXT("ue_agent_interface.control_rig.functions.v1"), TEXT("functions"), TEXT("function declaration writes are not silently applied; use graphs/graphs.json for existing function graph nodes")), OutError);
	UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("modular"), TEXT("modules.json")), UeAgentControlRigOps::MakeUnsupportedPlaceholderJson(TEXT("ue_agent_interface.control_rig.modules.v1"), TEXT("modules"), TEXT("modular rig module writes require Modular Rig controller support")), OutError);
	UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("modular"), TEXT("connections.json")), UeAgentControlRigOps::MakeUnsupportedPlaceholderJson(TEXT("ue_agent_interface.control_rig.module_connections.v1"), TEXT("module_connections"), TEXT("modular rig connection writes require Modular Rig controller support")), OutError);
	UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("modular"), TEXT("connectors.json")), UeAgentControlRigOps::MakeUnsupportedPlaceholderJson(TEXT("ue_agent_interface.control_rig.module_connectors.v1"), TEXT("module_connectors"), TEXT("connector writes are validated as unsupported_apply instead of ignored")), OutError);
	UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("presets"), TEXT("foot_ground_fbik.json")), UeAgentControlRigOps::MakeUnsupportedPlaceholderJson(TEXT("ue_agent_interface.control_rig.preset.foot_ground_fbik.v1"), TEXT("foot_ground_fbik"), TEXT("preset documents expected graph/hierarchy layout; apply only through explicit graphs/hierarchy files")), OutError);
	UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("raw_properties.json")), UeAgentControlRigOps::MakeUnsupportedPlaceholderJson(TEXT("ue_agent_interface.raw_properties.v1"), TEXT("raw_properties"), TEXT("raw fallback writes must go through asset_apply_property_json until Control Rig conflict checks are complete")), OutError);
	UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("readonly_properties.json")), UeAgentControlRigOps::MakeUnsupportedPlaceholderJson(TEXT("ue_agent_interface.readonly_properties.v1"), TEXT("readonly_properties"), TEXT("readonly")), OutError);

	UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("validation"), TEXT("coverage_report.json")), UeAgentControlRigOps::MakeCoverageJson(), OutError);
	TSharedPtr<FJsonObject> DiagnosticsJson = MakeShared<FJsonObject>();
	DiagnosticsJson->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.control_rig.diagnostics.v1"));
	DiagnosticsJson->SetArrayField(TEXT("issues"), {});
	UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("validation"), TEXT("diagnostics.json")), DiagnosticsJson, OutError);
	UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("validation"), TEXT("compile_report.json")), UeAgentControlRigOps::MakeCompileReportJson(Blueprint), OutError);
	TSharedPtr<FJsonObject> ReadbackDiffJson = MakeShared<FJsonObject>();
	ReadbackDiffJson->SetBoolField(TEXT("has_diff"), false);
	ReadbackDiffJson->SetArrayField(TEXT("diffs"), {});
	UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("validation"), TEXT("readback_diff.json")), ReadbackDiffJson, OutError);
	UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("validation"), TEXT("runtime_probe_report.json")), UeAgentControlRigOps::MakeRuntimeProbeReportJson(), OutError);
	UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("validation"), TEXT("rigvm_unit_registry.json")), UeAgentControlRigOps::MakeRigVMUnitRegistryJson(), OutError);

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("asset_path"), UeAgentControlRigOps::NormalizeAssetPath(AssetPath));
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.control_rig.folder.v1"));
	OutData->SetObjectField(TEXT("coverage"), UeAgentControlRigOps::MakeCoverageJson());
	return true;
}

bool FUeAgentHttpServer::CmdControlRigValidateFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FUeAgentRequestContext ValidateCtx = Ctx;
	ValidateCtx.Params->SetBoolField(TEXT("validate_only"), true);
	ValidateCtx.Params->SetBoolField(TEXT("dry_run"), true);
	return CmdControlRigApplyFolder(ValidateCtx, OutData, OutError);
}

bool FUeAgentHttpServer::CmdControlRigApplyFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString FolderPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderPath) || FolderPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("folder_path_required");
		return false;
	}
	FolderPath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(FolderPath);

	bool bValidateOnly = false;
	bool bDryRun = false;
	bool bCreateIfMissing = false;
	bool bSaveAfterApply = false;
	bool bCompileAfterApply = true;
	JsonTryGetBool(Ctx.Params, TEXT("validate_only"), bValidateOnly);
	JsonTryGetBool(Ctx.Params, TEXT("dry_run"), bDryRun);
	JsonTryGetBool(Ctx.Params, TEXT("create_if_missing"), bCreateIfMissing);
	JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSaveAfterApply);
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_apply"), bCompileAfterApply);

	TArray<TSharedPtr<FJsonValue>> Issues;
	TSharedPtr<FJsonObject> AssetJson;
	UeAgentControlRigOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("asset.json")), AssetJson, Issues, OutError, false);
	if (AssetJson.IsValid())
	{
		UeAgentJsonDiagnostics::WarnUnknownFields(AssetJson, TEXT("asset.json"), { TEXT("schema"), TEXT("asset_path"), TEXT("object_path"), TEXT("asset_class"), TEXT("control_rig_type"), TEXT("control_rig_class"), TEXT("generated_class"), TEXT("preview_skeletal_mesh"), TEXT("shape_libraries"), TEXT("hierarchy"), TEXT("graphs"), TEXT("graph_count"), TEXT("blueprint_status") }, Issues);
	}

	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) && AssetJson.IsValid())
	{
		UeAgentJsonDiagnostics::ReadStringField(AssetJson, TEXT("asset_path"), TEXT("asset.json.asset_path"), AssetPath, Issues, true);
	}
	if (AssetPath.TrimStartAndEnd().IsEmpty())
	{
		UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("asset_path_required"), TEXT("asset.json.asset_path"), TEXT("Control Rig asset_path is required."));
	}

	UControlRigBlueprint* Blueprint = AssetPath.IsEmpty() ? nullptr : UeAgentControlRigOps::LoadControlRigBlueprint(AssetPath);
	if (!Blueprint && bCreateIfMissing && !bValidateOnly && !bDryRun && !AssetPath.IsEmpty())
	{
		FString RigTypeText;
		if (AssetJson.IsValid())
		{
			AssetJson->TryGetStringField(TEXT("control_rig_type"), RigTypeText);
		}
		EControlRigType RigType = EControlRigType::IndependentRig;
		UeAgentControlRigOps::ParseControlRigTypeText(RigTypeText, RigType);
		Blueprint = UeAgentControlRigOps::CreateControlRigAsset(AssetPath, RigType == EControlRigType::ModularRig, FString(), false, OutError);
	}
	if (!Blueprint && !bValidateOnly && !bDryRun)
	{
		UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("control_rig_not_found"), TEXT("asset.json.asset_path"), TEXT("Control Rig asset was not found and create_if_missing is false."));
	}

	TSharedPtr<FJsonObject> PreviewJson;
	UeAgentControlRigOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("settings"), TEXT("preview.json")), PreviewJson, Issues, OutError, true);
	TSharedPtr<FJsonObject> ControlRigSettingsJson;
	UeAgentControlRigOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("settings"), TEXT("control_rig.json")), ControlRigSettingsJson, Issues, OutError, true);
	if (ControlRigSettingsJson.IsValid())
	{
		UeAgentJsonDiagnostics::WarnUnknownFields(ControlRigSettingsJson, TEXT("settings/control_rig.json"), { TEXT("schema"), TEXT("profile"), TEXT("apply_policy"), TEXT("control_rig_type"), TEXT("auto_recompile"), TEXT("event_queue") }, Issues);
	}
	TSharedPtr<FJsonObject> CompileSettingsJson;
	UeAgentControlRigOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("settings"), TEXT("vm_compile_settings.json")), CompileSettingsJson, Issues, OutError, true);
	if (CompileSettingsJson.IsValid())
	{
		UeAgentJsonDiagnostics::WarnUnknownFields(CompileSettingsJson, TEXT("settings/vm_compile_settings.json"), { TEXT("schema"), TEXT("apply_policy"), TEXT("compile_after_apply_default") }, Issues);
	}
	TSharedPtr<FJsonObject> ShapeRefsJson;
	UeAgentControlRigOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("shape_libraries"), TEXT("references.json")), ShapeRefsJson, Issues, OutError, true);
	TSharedPtr<FJsonObject> BonesJson;
	UeAgentControlRigOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("hierarchy"), TEXT("bones.json")), BonesJson, Issues, OutError, true);
	TSharedPtr<FJsonObject> NullsJson;
	UeAgentControlRigOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("hierarchy"), TEXT("nulls.json")), NullsJson, Issues, OutError, true);
	TSharedPtr<FJsonObject> ControlsJson;
	UeAgentControlRigOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("hierarchy"), TEXT("controls.json")), ControlsJson, Issues, OutError, true);
	TSharedPtr<FJsonObject> CurvesJson;
	UeAgentControlRigOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("hierarchy"), TEXT("curves.json")), CurvesJson, Issues, OutError, true);
	TSharedPtr<FJsonObject> GraphsJson;
	UeAgentControlRigOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("graphs"), TEXT("graphs.json")), GraphsJson, Issues, OutError, true);
	TSharedPtr<FJsonObject> VariablesJson;
	UeAgentControlRigOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("variables"), TEXT("variables.json")), VariablesJson, Issues, OutError, true);
	TSharedPtr<FJsonObject> ExposedPropertiesJson;
	UeAgentControlRigOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("variables"), TEXT("exposed_properties.json")), ExposedPropertiesJson, Issues, OutError, true);
	TSharedPtr<FJsonObject> FunctionsJson;
	UeAgentControlRigOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("functions"), TEXT("functions.json")), FunctionsJson, Issues, OutError, true);
	TSharedPtr<FJsonObject> ModulesJson;
	UeAgentControlRigOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("modular"), TEXT("modules.json")), ModulesJson, Issues, OutError, true);
	TSharedPtr<FJsonObject> ModuleConnectionsJson;
	UeAgentControlRigOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("modular"), TEXT("connections.json")), ModuleConnectionsJson, Issues, OutError, true);
	TSharedPtr<FJsonObject> ModuleConnectorsJson;
	UeAgentControlRigOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("modular"), TEXT("connectors.json")), ModuleConnectorsJson, Issues, OutError, true);
	TSharedPtr<FJsonObject> RawPropertiesJson;
	UeAgentControlRigOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("raw_properties.json")), RawPropertiesJson, Issues, OutError, true);
	TSharedPtr<FJsonObject> ReadonlyPropertiesJson;
	UeAgentControlRigOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("readonly_properties.json")), ReadonlyPropertiesJson, Issues, OutError, true);

	UeAgentControlRigOps::AddUnsupportedApplyIssueIfNeeded(ExposedPropertiesJson, TEXT("variables/exposed_properties.json"), TEXT("Control Rig exposed properties"), Issues);
	UeAgentControlRigOps::AddUnsupportedApplyIssueIfNeeded(FunctionsJson, TEXT("functions/functions.json"), TEXT("Control Rig functions"), Issues);
	UeAgentControlRigOps::AddUnsupportedApplyIssueIfNeeded(ModulesJson, TEXT("modular/modules.json"), TEXT("Modular Rig modules"), Issues);
	UeAgentControlRigOps::AddUnsupportedApplyIssueIfNeeded(ModuleConnectionsJson, TEXT("modular/connections.json"), TEXT("Modular Rig connections"), Issues);
	UeAgentControlRigOps::AddUnsupportedApplyIssueIfNeeded(ModuleConnectorsJson, TEXT("modular/connectors.json"), TEXT("Modular Rig connectors"), Issues);
	UeAgentControlRigOps::AddUnsupportedApplyIssueIfNeeded(RawPropertiesJson, TEXT("raw_properties.json"), TEXT("Control Rig raw properties"), Issues);
	UeAgentControlRigOps::AddUnsupportedApplyIssueIfNeeded(ReadonlyPropertiesJson, TEXT("readonly_properties.json"), TEXT("Control Rig readonly properties"), Issues);

	TSharedPtr<FJsonObject> ApplyCompileReport;
	UeAgentControlRigOps::FControlRigVariableApplyStats VariableStats;
	if (!bValidateOnly && !bDryRun && Blueprint && !UeAgentControlRigOps::HasBlockingIssue(Issues))
	{
		UeAgentControlRigOps::ApplyPreviewJson(Blueprint, PreviewJson, TEXT("settings/preview.json"), Issues);
		UeAgentControlRigOps::ApplyShapeLibrariesJson(Blueprint, ShapeRefsJson, TEXT("shape_libraries/references.json"), Issues);
		UeAgentControlRigOps::ApplyVariablesJson(Blueprint, VariablesJson, TEXT("variables/variables.json"), Issues, VariableStats);
		UeAgentControlRigOps::ApplyBonesJson(Blueprint, BonesJson, TEXT("hierarchy/bones.json"), Issues);
		UeAgentControlRigOps::ApplyNullsJson(Blueprint, NullsJson, TEXT("hierarchy/nulls.json"), Issues);
		UeAgentControlRigOps::ApplyControlsJson(Blueprint, ControlsJson, TEXT("hierarchy/controls.json"), Issues);
		UeAgentControlRigOps::ApplyCurvesJson(Blueprint, CurvesJson, TEXT("hierarchy/curves.json"), Issues);
		UeAgentControlRigOps::ApplyGraphsJson(Blueprint, GraphsJson, TEXT("graphs/graphs.json"), Issues);
		UeAgentControlRigOps::MarkControlRigModified(Blueprint);

		if (bCompileAfterApply && !UeAgentControlRigOps::HasBlockingIssue(Issues))
		{
			FCompilerResultsLog CompilerLog;
			FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &CompilerLog);
			ApplyCompileReport = UeAgentControlRigOps::MakeCompileReportJson(Blueprint, &CompilerLog);
			if (CompilerLog.NumErrors > 0 || Blueprint->Status == BS_Error)
			{
				UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("control_rig_compile_failed"), TEXT("compile"), TEXT("Control Rig compile failed after folder apply. See compile_report in the response."));
			}
		}

		if (bSaveAfterApply && !UeAgentControlRigOps::SaveAssetPackage(Blueprint, OutError))
		{
			return false;
		}
	}

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetStringField(TEXT("asset_path"), UeAgentControlRigOps::NormalizeAssetPath(AssetPath));
	OutData->SetBoolField(TEXT("validate_only"), bValidateOnly || bDryRun);
	OutData->SetBoolField(TEXT("valid"), !UeAgentControlRigOps::HasBlockingIssue(Issues));
	OutData->SetBoolField(TEXT("compiled"), Blueprint ? Blueprint->Status != BS_Error : false);
	OutData->SetBoolField(TEXT("compile_after_apply"), bCompileAfterApply);
	OutData->SetNumberField(TEXT("json_issue_count"), Issues.Num());
	OutData->SetNumberField(TEXT("error_count"), UeAgentControlRigOps::CountIssuesBySeverity(Issues, TEXT("error")));
	OutData->SetNumberField(TEXT("warning_count"), UeAgentControlRigOps::CountIssuesBySeverity(Issues, TEXT("warning")));
	OutData->SetArrayField(TEXT("issues"), Issues);
	TSharedPtr<FJsonObject> AppliedObj = MakeShared<FJsonObject>();
	AppliedObj->SetNumberField(TEXT("variables_added"), VariableStats.Added);
	AppliedObj->SetNumberField(TEXT("variable_defaults_updated"), VariableStats.DefaultsUpdated);
	AppliedObj->SetNumberField(TEXT("variable_flags_updated"), VariableStats.FlagsUpdated);
	AppliedObj->SetNumberField(TEXT("variables_existing"), VariableStats.Existing);
	OutData->SetObjectField(TEXT("applied"), AppliedObj);
	OutData->SetObjectField(TEXT("coverage"), UeAgentControlRigOps::MakeCoverageJson());
	OutData->SetObjectField(TEXT("compile_report"), ApplyCompileReport.IsValid() ? ApplyCompileReport : UeAgentControlRigOps::MakeCompileReportJson(Blueprint));
	if (Blueprint)
	{
		OutData->SetObjectField(TEXT("readback"), UeAgentControlRigOps::BuildControlRigInfo(Blueprint));
	}
	return !UeAgentControlRigOps::HasBlockingIssue(Issues);
}

bool FUeAgentHttpServer::CmdControlRigCompile(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("asset_path_required");
		return false;
	}

	UControlRigBlueprint* Blueprint = UeAgentControlRigOps::LoadControlRigBlueprint(AssetPath);
	if (!Blueprint)
	{
		OutError = TEXT("control_rig_not_found");
		return false;
	}

	FCompilerResultsLog CompilerLog;
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &CompilerLog);
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("asset_path"), UeAgentControlRigOps::NormalizeAssetPath(AssetPath));
	OutData->SetNumberField(TEXT("blueprint_status"), static_cast<int32>(Blueprint->Status));
	OutData->SetBoolField(TEXT("compiled"), Blueprint->Status != BS_Error);
	OutData->SetNumberField(TEXT("error_count"), CompilerLog.NumErrors);
	OutData->SetNumberField(TEXT("warning_count"), CompilerLog.NumWarnings);
	OutData->SetObjectField(TEXT("compile_report"), UeAgentControlRigOps::MakeCompileReportJson(Blueprint, &CompilerLog));
	OutData->SetArrayField(TEXT("issues"), {});
	return Blueprint->Status != BS_Error;
}

bool FUeAgentHttpServer::CmdControlRigGetCompileLog(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (!CmdControlRigCompile(Ctx, OutData, OutError))
	{
		return false;
	}
	OutData->SetStringField(TEXT("note"), TEXT("Control Rig compile status and compiler messages are returned through compile_report."));
	return true;
}

bool FUeAgentHttpServer::CmdControlRigOpenEditor(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("asset_path_required");
		return false;
	}
	if (!UeAgentControlRigOps::LoadControlRigBlueprint(AssetPath))
	{
		OutError = TEXT("control_rig_not_found");
		return false;
	}
	if (!CmdOpenAssetEditor(Ctx, OutData, OutError))
	{
		return false;
	}
	OutData->SetStringField(TEXT("asset_kind"), TEXT("control_rig"));
	return true;
}

static FUeAgentRequestContext MakeControlRigBlueprintProxyContext(const FUeAgentRequestContext& Ctx)
{
	FUeAgentRequestContext Proxy = Ctx;
	Proxy.Params = MakeShared<FJsonObject>();
	if (Ctx.Params.IsValid())
	{
		Proxy.Params->Values = Ctx.Params->Values;
	}

	FString GraphNameOrPath;
	if (Proxy.Params.IsValid()
		&& !Proxy.Params->HasField(TEXT("graph_name"))
		&& Proxy.Params->TryGetStringField(TEXT("graph_name_or_path"), GraphNameOrPath)
		&& !GraphNameOrPath.TrimStartAndEnd().IsEmpty())
	{
		Proxy.Params->SetStringField(TEXT("graph_name"), GraphNameOrPath);
	}
	return Proxy;
}

bool FUeAgentHttpServer::CmdControlRigGraphGetView(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FUeAgentRequestContext Proxy = MakeControlRigBlueprintProxyContext(Ctx);
	if (!CmdBlueprintGraphGetView(Proxy, OutData, OutError))
	{
		return false;
	}
	OutData->SetStringField(TEXT("asset_kind"), TEXT("control_rig"));
	return true;
}

bool FUeAgentHttpServer::CmdControlRigGraphSetView(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FUeAgentRequestContext Proxy = MakeControlRigBlueprintProxyContext(Ctx);
	if (!CmdBlueprintGraphSetView(Proxy, OutData, OutError))
	{
		return false;
	}
	OutData->SetStringField(TEXT("asset_kind"), TEXT("control_rig"));
	return true;
}

bool FUeAgentHttpServer::CmdControlRigViewportGetCamera(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (!CmdBlueprintViewportGetCamera(Ctx, OutData, OutError))
	{
		return false;
	}
	OutData->SetStringField(TEXT("asset_kind"), TEXT("control_rig"));
	return true;
}

bool FUeAgentHttpServer::CmdControlRigViewportSetCamera(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (!CmdBlueprintViewportSetCamera(Ctx, OutData, OutError))
	{
		return false;
	}
	OutData->SetStringField(TEXT("asset_kind"), TEXT("control_rig"));
	return true;
}

bool FUeAgentHttpServer::CmdControlRigScreenshot(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FUeAgentRequestContext Proxy = MakeControlRigBlueprintProxyContext(Ctx);
	if (!CmdBlueprintScreenshot(Proxy, OutData, OutError))
	{
		return false;
	}
	OutData->SetStringField(TEXT("asset_kind"), TEXT("control_rig"));
	return true;
}

static void ReadStringArrayParam(const TSharedPtr<FJsonObject>& Params, const FString& FieldName, TArray<FString>& OutItems)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Params.IsValid() || !Params->TryGetArrayField(FieldName, Values) || !Values)
	{
		return;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		FString Text;
		if (Value.IsValid() && Value->TryGetString(Text) && !Text.TrimStartAndEnd().IsEmpty())
		{
			OutItems.Add(Text);
		}
	}
}

static TSharedPtr<FJsonObject> MakeRuntimeElementSample(URigHierarchy* Hierarchy, const FRigElementKey& Key)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("name"), Key.Name.ToString());
	Obj->SetStringField(TEXT("type"), UeAgentControlRigOps::ElementTypeToString(Key.Type));
	if (!Hierarchy || !Hierarchy->Contains(Key))
	{
		Obj->SetBoolField(TEXT("found"), false);
		return Obj;
	}
	Obj->SetBoolField(TEXT("found"), true);
	Obj->SetObjectField(TEXT("global_transform"), UeAgentControlRigOps::MakeTransformJson(Hierarchy->GetGlobalTransform(Key, false)));
	Obj->SetObjectField(TEXT("local_transform"), UeAgentControlRigOps::MakeTransformJson(Hierarchy->GetLocalTransform(Key, false)));
	Obj->SetObjectField(TEXT("initial_global_transform"), UeAgentControlRigOps::MakeTransformJson(Hierarchy->GetGlobalTransform(Key, true)));
	return Obj;
}

bool FUeAgentHttpServer::CmdControlRigRuntimeProbe(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("asset_path_required");
		return false;
	}

	UControlRigBlueprint* Blueprint = UeAgentControlRigOps::LoadControlRigBlueprint(AssetPath);
	if (!Blueprint)
	{
		OutError = TEXT("control_rig_not_found");
		return false;
	}

	FCompilerResultsLog CompilerLog;
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &CompilerLog);
	if (CompilerLog.NumErrors > 0 || Blueprint->Status == BS_Error)
	{
		OutData = MakeShared<FJsonObject>();
		OutData->SetStringField(TEXT("asset_path"), UeAgentControlRigOps::NormalizeAssetPath(AssetPath));
		OutData->SetObjectField(TEXT("compile_report"), UeAgentControlRigOps::MakeCompileReportJson(Blueprint, &CompilerLog));
		OutError = TEXT("control_rig_compile_failed");
		return false;
	}

	UClass* RigClass = Blueprint->GetControlRigClass();
	if (!RigClass || !RigClass->IsChildOf(UControlRig::StaticClass()))
	{
		OutError = TEXT("control_rig_class_missing");
		return false;
	}

	UControlRig* RuntimeRig = NewObject<UControlRig>(GetTransientPackage(), RigClass, NAME_None, RF_Transient);
	if (!RuntimeRig)
	{
		OutError = TEXT("control_rig_runtime_instance_failed");
		return false;
	}

	RuntimeRig->Initialize(true);
	RuntimeRig->RequestInit();

	TArray<TSharedPtr<FJsonValue>> ProbeIssues;
	TArray<FString> VariableNamesToSample;
	auto ApplyVariableInput = [&](const FString& VariableName, const TSharedPtr<FJsonValue>& Value, const FString& JsonPath)
	{
		if (VariableName.TrimStartAndEnd().IsEmpty())
		{
			UeAgentJsonDiagnostics::AddIssue(ProbeIssues, TEXT("error"), TEXT("control_rig_probe_variable_name_required"), JsonPath, TEXT("Runtime probe variable input requires a non-empty variable name."));
			return;
		}
		FString ValueText;
		if (!UeAgentControlRigOps::ReadJsonValueAsDefaultText(Value, ValueText))
		{
			UeAgentJsonDiagnostics::AddIssue(ProbeIssues, TEXT("error"), TEXT("control_rig_probe_variable_value_invalid"), JsonPath, TEXT("Runtime probe variable value must be a string, number, bool, vector object, or color object."));
			return;
		}
		FProperty* Property = FindFProperty<FProperty>(RigClass, FName(*VariableName));
		if (!Property)
		{
			UeAgentJsonDiagnostics::AddIssue(ProbeIssues, TEXT("error"), TEXT("control_rig_probe_variable_not_found"), JsonPath, FString::Printf(TEXT("Variable '%s' does not exist on the generated Control Rig class."), *VariableName));
			return;
		}
		void* ValuePtr = Property->ContainerPtrToValuePtr<void>(RuntimeRig);
		if (!ValuePtr || Property->ImportText_Direct(*ValueText, ValuePtr, RuntimeRig, PPF_None) == nullptr)
		{
			UeAgentJsonDiagnostics::AddIssue(ProbeIssues, TEXT("error"), TEXT("control_rig_probe_variable_import_failed"), JsonPath, FString::Printf(TEXT("Failed to import runtime value for variable '%s'."), *VariableName));
			return;
		}
		VariableNamesToSample.AddUnique(VariableName);
	};

	const TSharedPtr<FJsonObject>* VariableInputsObj = nullptr;
	if (Ctx.Params.IsValid() && Ctx.Params->TryGetObjectField(TEXT("variables"), VariableInputsObj) && VariableInputsObj && VariableInputsObj->IsValid())
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*VariableInputsObj)->Values)
		{
			ApplyVariableInput(Pair.Key, Pair.Value, FString::Printf(TEXT("variables.%s"), *Pair.Key));
		}
	}
	const TArray<TSharedPtr<FJsonValue>>* VariableInputsArray = nullptr;
	if (Ctx.Params.IsValid() && Ctx.Params->TryGetArrayField(TEXT("variable_inputs"), VariableInputsArray) && VariableInputsArray)
	{
		for (int32 VariableInputIndex = 0; VariableInputIndex < VariableInputsArray->Num(); ++VariableInputIndex)
		{
			TSharedPtr<FJsonObject> VariableInputObj;
			const FString VariableInputPath = FString::Printf(TEXT("variable_inputs[%d]"), VariableInputIndex);
			if (!UeAgentJsonDiagnostics::ReadObjectFromValue((*VariableInputsArray)[VariableInputIndex], VariableInputPath, VariableInputObj, ProbeIssues))
			{
				continue;
			}
			FString Name;
			UeAgentJsonDiagnostics::ReadStringField(VariableInputObj, TEXT("name"), VariableInputPath + TEXT(".name"), Name, ProbeIssues, true);
			const TSharedPtr<FJsonValue>* Value = UeAgentJsonDiagnostics::FindFieldValue(VariableInputObj, TEXT("value"));
			if (!Value)
			{
				Value = UeAgentJsonDiagnostics::FindFieldValue(VariableInputObj, TEXT("default_value"));
			}
			if (!Value)
			{
				UeAgentJsonDiagnostics::AddIssue(ProbeIssues, TEXT("error"), TEXT("control_rig_probe_variable_value_required"), VariableInputPath, TEXT("variable_inputs item requires value or default_value."));
				continue;
			}
			ApplyVariableInput(Name, *Value, VariableInputPath + TEXT(".value"));
		}
	}

	TArray<TSharedPtr<FJsonValue>> SupportedEventValues;
	for (const FName& SupportedEvent : RuntimeRig->GetSupportedEvents())
	{
		SupportedEventValues.Add(MakeShared<FJsonValueString>(SupportedEvent.ToString()));
	}

	bool bExecuteConstruction = true;
	JsonTryGetBool(Ctx.Params, TEXT("execute_construction"), bExecuteConstruction);
	bool bConstructionOk = true;
	bool bConstructionSkipped = false;
	if (bExecuteConstruction)
	{
		if (RuntimeRig->SupportsEvent(FRigUnit_PrepareForExecution::EventName))
		{
			bConstructionOk = RuntimeRig->Execute(FRigUnit_PrepareForExecution::EventName);
		}
		else
		{
			bConstructionSkipped = true;
		}
	}

	FString EventName = TEXT("Forwards Solve");
	JsonTryGetString(Ctx.Params, TEXT("event_name"), EventName);
	int32 Frames = 1;
	double FramesNum = static_cast<double>(Frames);
	if (JsonTryGetNumber(Ctx.Params, TEXT("frames"), FramesNum))
	{
		Frames = FMath::Clamp(FMath::RoundToInt(FramesNum), 0, 1000);
	}

	bool bExecutionOk = true;
	bool bExecutionSkipped = false;
	const FName EventFName(*EventName);
	if (Frames > 0 && RuntimeRig->SupportsEvent(EventFName))
	{
		for (int32 FrameIndex = 0; FrameIndex < Frames; ++FrameIndex)
		{
			bExecutionOk = RuntimeRig->Execute(EventFName) && bExecutionOk;
		}
	}
	else if (Frames > 0)
	{
		bExecutionSkipped = true;
	}

	URigHierarchy* Hierarchy = RuntimeRig->GetHierarchy();
	TArray<FString> SampleBones;
	TArray<FString> SampleControls;
	TArray<FString> SampleVariables;
	ReadStringArrayParam(Ctx.Params, TEXT("sample_bones"), SampleBones);
	ReadStringArrayParam(Ctx.Params, TEXT("sample_controls"), SampleControls);
	ReadStringArrayParam(Ctx.Params, TEXT("sample_variables"), SampleVariables);

	int32 MaxSamples = 64;
	double MaxSamplesNum = static_cast<double>(MaxSamples);
	if (JsonTryGetNumber(Ctx.Params, TEXT("max_samples"), MaxSamplesNum))
	{
		MaxSamples = FMath::Clamp(FMath::RoundToInt(MaxSamplesNum), 1, 512);
	}

	if (Hierarchy)
	{
		if (SampleBones.Num() == 0)
		{
			for (const FRigElementKey& Key : Hierarchy->GetBoneKeys(true))
			{
				if (SampleBones.Num() >= MaxSamples)
				{
					break;
				}
				SampleBones.Add(Key.Name.ToString());
			}
		}
		if (SampleControls.Num() == 0)
		{
			for (const FRigElementKey& Key : Hierarchy->GetControlKeys(true))
			{
				if (SampleControls.Num() >= MaxSamples)
				{
					break;
				}
				SampleControls.Add(Key.Name.ToString());
			}
		}
	}
	if (SampleVariables.Num() == 0)
	{
		SampleVariables = VariableNamesToSample;
		if (SampleVariables.Num() == 0)
		{
			for (const FBPVariableDescription& VarDesc : Blueprint->NewVariables)
			{
				if (SampleVariables.Num() >= MaxSamples)
				{
					break;
				}
				SampleVariables.Add(VarDesc.VarName.ToString());
			}
		}
	}

	TArray<TSharedPtr<FJsonValue>> BoneValues;
	for (const FString& BoneName : SampleBones)
	{
		BoneValues.Add(MakeShared<FJsonValueObject>(MakeRuntimeElementSample(Hierarchy, FRigElementKey(FName(*BoneName), ERigElementType::Bone))));
	}
	TArray<TSharedPtr<FJsonValue>> ControlValues;
	for (const FString& ControlName : SampleControls)
	{
		ControlValues.Add(MakeShared<FJsonValueObject>(MakeRuntimeElementSample(Hierarchy, FRigElementKey(FName(*ControlName), ERigElementType::Control))));
	}
	TArray<TSharedPtr<FJsonValue>> VariableValues;
	for (const FString& VariableName : SampleVariables)
	{
		TSharedPtr<FJsonObject> VariableObj = MakeShared<FJsonObject>();
		VariableObj->SetStringField(TEXT("name"), VariableName);
		FProperty* Property = FindFProperty<FProperty>(RigClass, FName(*VariableName));
		if (!Property)
		{
			VariableObj->SetBoolField(TEXT("found"), false);
		}
		else
		{
			VariableObj->SetBoolField(TEXT("found"), true);
			FString ExtendedType;
			VariableObj->SetStringField(TEXT("cpp_type"), Property->GetCPPType(&ExtendedType) + ExtendedType);
			FString ValueText;
			if (void* ValuePtr = Property->ContainerPtrToValuePtr<void>(RuntimeRig))
			{
				Property->ExportTextItem_Direct(ValueText, ValuePtr, ValuePtr, RuntimeRig, PPF_None);
			}
			VariableObj->SetStringField(TEXT("value_text"), ValueText);
		}
		VariableValues.Add(MakeShared<FJsonValueObject>(VariableObj));
	}

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("asset_path"), UeAgentControlRigOps::NormalizeAssetPath(AssetPath));
	OutData->SetStringField(TEXT("probe_mode"), TEXT("transient_control_rig_instance"));
	OutData->SetArrayField(TEXT("supported_events"), SupportedEventValues);
	OutData->SetBoolField(TEXT("construction_executed"), bExecuteConstruction);
	OutData->SetBoolField(TEXT("construction_skipped"), bConstructionSkipped);
	OutData->SetBoolField(TEXT("construction_ok"), bConstructionOk);
	OutData->SetStringField(TEXT("event_name"), EventName);
	OutData->SetNumberField(TEXT("frames"), Frames);
	OutData->SetBoolField(TEXT("execution_skipped"), bExecutionSkipped);
	OutData->SetBoolField(TEXT("execution_ok"), bExecutionOk);
	OutData->SetObjectField(TEXT("compile_report"), UeAgentControlRigOps::MakeCompileReportJson(Blueprint, &CompilerLog));
	OutData->SetArrayField(TEXT("issues"), ProbeIssues);
	OutData->SetNumberField(TEXT("error_count"), UeAgentControlRigOps::CountIssuesBySeverity(ProbeIssues, TEXT("error")));
	OutData->SetNumberField(TEXT("warning_count"), UeAgentControlRigOps::CountIssuesBySeverity(ProbeIssues, TEXT("warning")));
	OutData->SetArrayField(TEXT("bones"), BoneValues);
	OutData->SetArrayField(TEXT("controls"), ControlValues);
	OutData->SetArrayField(TEXT("variables"), VariableValues);
	return bConstructionOk && bExecutionOk && !UeAgentControlRigOps::HasBlockingIssue(ProbeIssues);
}

static bool ParseGuidParam(const TSharedPtr<FJsonObject>& Params, const FString& FieldName, FGuid& OutGuid)
{
	FString Text;
	if (!Params.IsValid() || !Params->TryGetStringField(FieldName, Text))
	{
		return false;
	}
	return FGuid::Parse(Text, OutGuid);
}

struct FUeAgentControlRigBakeBindingInfo
{
	bool bBindingExists = false;
	bool bPlayerCreated = false;
	int32 BoundObjectCount = 0;
	TArray<FString> BoundObjectPaths;
	FString FirstSkeletalMeshComponentPath;
	USkeletalMesh* SkeletalMesh = nullptr;
	USkeleton* Skeleton = nullptr;
	FString Error;
};

static TSharedPtr<FJsonObject> MakeBakeBindingInfoJson(const FUeAgentControlRigBakeBindingInfo& Info)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetBoolField(TEXT("binding_exists"), Info.bBindingExists);
	Obj->SetBoolField(TEXT("player_created"), Info.bPlayerCreated);
	Obj->SetNumberField(TEXT("bound_object_count"), Info.BoundObjectCount);
	TArray<TSharedPtr<FJsonValue>> BoundObjects;
	for (const FString& Path : Info.BoundObjectPaths)
	{
		BoundObjects.Add(MakeShared<FJsonValueString>(Path));
	}
	Obj->SetArrayField(TEXT("bound_objects"), BoundObjects);
	Obj->SetStringField(TEXT("skeletal_mesh_component"), Info.FirstSkeletalMeshComponentPath);
	Obj->SetStringField(TEXT("skeletal_mesh"), Info.SkeletalMesh ? Info.SkeletalMesh->GetPathName() : TEXT(""));
	Obj->SetStringField(TEXT("skeleton"), Info.Skeleton ? Info.Skeleton->GetPathName() : TEXT(""));
	Obj->SetStringField(TEXT("error"), Info.Error);
	return Obj;
}

static USkeletalMeshComponent* FindSkeletalMeshComponentInBoundObject(UObject* BoundObject)
{
	if (!BoundObject)
	{
		return nullptr;
	}

	if (AActor* Actor = Cast<AActor>(BoundObject))
	{
		for (UActorComponent* Component : Actor->GetComponents())
		{
			if (USkeletalMeshComponent* SkeletalMeshComponent = Cast<USkeletalMeshComponent>(Component))
			{
				if (SkeletalMeshComponent->GetSkeletalMeshAsset())
				{
					return SkeletalMeshComponent;
				}
			}
		}
	}
	else if (USkeletalMeshComponent* SkeletalMeshComponent = Cast<USkeletalMeshComponent>(BoundObject))
	{
		if (SkeletalMeshComponent->GetSkeletalMeshAsset())
		{
			return SkeletalMeshComponent;
		}
	}

	return nullptr;
}

static FUeAgentControlRigBakeBindingInfo InspectBakeBinding(UWorld* World, ULevelSequence* Sequence, const FGuid& BindingGuid)
{
	FUeAgentControlRigBakeBindingInfo Info;
	if (!World)
	{
		Info.Error = TEXT("editor_world_not_found");
		return Info;
	}
	if (!Sequence || !Sequence->GetMovieScene())
	{
		Info.Error = TEXT("level_sequence_not_found");
		return Info;
	}

	Info.bBindingExists = Sequence->GetMovieScene()->FindBinding(BindingGuid) != nullptr;
	if (!Info.bBindingExists)
	{
		Info.Error = TEXT("binding_not_found_in_level_sequence");
		return Info;
	}

	ALevelSequenceActor* OutActor = nullptr;
	FMovieSceneSequencePlaybackSettings Settings;
	ULevelSequencePlayer* Player = ULevelSequencePlayer::CreateLevelSequencePlayer(World, Sequence, Settings, OutActor);
	Info.bPlayerCreated = Player != nullptr;
	if (!Player)
	{
		Info.Error = TEXT("level_sequence_player_create_failed");
		return Info;
	}

	if (UMovieScene* MovieScene = Sequence->GetMovieScene())
	{
		const FFrameNumber StartFrame = MovieScene->GetPlaybackRange().GetLowerBoundValue();
		Player->SetPlaybackPosition(FMovieSceneSequencePlaybackParams(FFrameTime(StartFrame), EUpdatePositionMethod::Jump));
	}

	for (TWeakObjectPtr<> RuntimeObject : Player->FindBoundObjects(BindingGuid, MovieSceneSequenceID::Root))
	{
		UObject* BoundObject = RuntimeObject.Get();
		if (!BoundObject)
		{
			continue;
		}

		++Info.BoundObjectCount;
		Info.BoundObjectPaths.Add(BoundObject->GetPathName());
		if (!Info.SkeletalMesh)
		{
			if (USkeletalMeshComponent* SkeletalMeshComponent = FindSkeletalMeshComponentInBoundObject(BoundObject))
			{
				Info.FirstSkeletalMeshComponentPath = SkeletalMeshComponent->GetPathName();
				Info.SkeletalMesh = SkeletalMeshComponent->GetSkeletalMeshAsset();
				Info.Skeleton = Info.SkeletalMesh ? Info.SkeletalMesh->GetSkeleton() : nullptr;
			}
		}
	}

	if (!Info.SkeletalMesh || !Info.Skeleton)
	{
		Info.Error = Info.BoundObjectCount > 0 ? TEXT("bound_object_has_no_skeletal_mesh") : TEXT("binding_resolved_no_objects");
	}

	Player->Stop();
	if (OutActor)
	{
		World->DestroyActor(OutActor);
	}
	return Info;
}

static UAnimSequence* CreateAnimSequenceAssetForBake(const FString& OutputAssetPath, USkeleton* Skeleton, USkeletalMesh* PreviewMesh, FString& OutError)
{
	const FString NormalizedPath = UeAgentControlRigOps::NormalizeAssetPath(OutputAssetPath);
	if (!FPackageName::IsValidLongPackageName(NormalizedPath))
	{
		OutError = TEXT("invalid_output_anim_sequence_path");
		return nullptr;
	}
	if (!Skeleton)
	{
		OutError = TEXT("target_skeleton_required_for_new_anim_sequence");
		return nullptr;
	}

	const FString AssetName = FPackageName::GetLongPackageAssetName(NormalizedPath);
	if (AssetName.IsEmpty())
	{
		OutError = TEXT("invalid_output_anim_sequence_path");
		return nullptr;
	}

	UPackage* Package = CreatePackage(*NormalizedPath);
	if (!Package)
	{
		OutError = TEXT("create_anim_sequence_package_failed");
		return nullptr;
	}
	Package->FullyLoad();
	if (UObject* ExistingObject = FindObject<UObject>(Package, *AssetName))
	{
		OutError = ExistingObject->IsA<UAnimSequence>() ? TEXT("anim_sequence_already_exists") : TEXT("output_asset_exists_with_different_class");
		return nullptr;
	}

	UAnimSequence* AnimSequence = NewObject<UAnimSequence>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
	if (!AnimSequence)
	{
		OutError = TEXT("create_anim_sequence_failed");
		return nullptr;
	}

	AnimSequence->SetSkeleton(Skeleton);
	if (PreviewMesh)
	{
		AnimSequence->SetPreviewMesh(PreviewMesh, true);
	}
	FAssetRegistryModule::AssetCreated(AnimSequence);
	Package->MarkPackageDirty();
	return AnimSequence;
}

bool FUeAgentHttpServer::CmdControlRigBakeToAnimation(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString SequencePath;
	FString OutputAnimSequencePath;
	if (!JsonTryGetString(Ctx.Params, TEXT("sequence_path"), SequencePath) || SequencePath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("sequence_path_required");
		return false;
	}
	if (!JsonTryGetString(Ctx.Params, TEXT("output_anim_sequence"), OutputAnimSequencePath) || OutputAnimSequencePath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("output_anim_sequence_required");
		return false;
	}
	FGuid BindingGuid;
	if (!ParseGuidParam(Ctx.Params, TEXT("binding_id"), BindingGuid) && !ParseGuidParam(Ctx.Params, TEXT("binding_guid"), BindingGuid))
	{
		OutError = TEXT("binding_id_required");
		return false;
	}

	ULevelSequence* Sequence = UeAgentControlRigOps::LoadAsset<ULevelSequence>(SequencePath);
	if (!Sequence)
	{
		OutError = TEXT("level_sequence_not_found");
		return false;
	}

	FString WorldError;
	UWorld* World = GetEditorWorld(WorldError);
	if (!World)
	{
		OutError = WorldError.IsEmpty() ? TEXT("editor_world_not_found") : WorldError;
		return false;
	}

	const FUeAgentControlRigBakeBindingInfo BindingInfo = InspectBakeBinding(World, Sequence, BindingGuid);
	if (!BindingInfo.Error.IsEmpty())
	{
		OutData = MakeShared<FJsonObject>();
		OutData->SetStringField(TEXT("sequence_path"), Sequence->GetOutermost()->GetName());
		OutData->SetStringField(TEXT("output_anim_sequence"), UeAgentControlRigOps::NormalizeAssetPath(OutputAnimSequencePath));
		OutData->SetStringField(TEXT("binding_id"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphens));
		OutData->SetObjectField(TEXT("binding_preflight"), MakeBakeBindingInfoJson(BindingInfo));
		OutError = BindingInfo.Error;
		return false;
	}

	UObject* ExistingOutputObject = UeAgentControlRigOps::LoadAssetObject(OutputAnimSequencePath);
	UAnimSequence* AnimSequence = Cast<UAnimSequence>(ExistingOutputObject);
	if (ExistingOutputObject && !AnimSequence)
	{
		OutData = MakeShared<FJsonObject>();
		OutData->SetStringField(TEXT("sequence_path"), Sequence->GetOutermost()->GetName());
		OutData->SetStringField(TEXT("output_anim_sequence"), ExistingOutputObject->GetPathName());
		OutData->SetStringField(TEXT("output_asset_class"), ExistingOutputObject->GetClass()->GetPathName());
		OutData->SetStringField(TEXT("binding_id"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphens));
		OutData->SetObjectField(TEXT("binding_preflight"), MakeBakeBindingInfoJson(BindingInfo));
		OutError = TEXT("output_asset_exists_with_different_class");
		return false;
	}

	bool bCreated = false;
	if (!AnimSequence)
	{
		const FString OutputAssetPath = UeAgentControlRigOps::NormalizeAssetPath(OutputAnimSequencePath);
		if (!FPackageName::IsValidLongPackageName(OutputAssetPath))
		{
			OutError = TEXT("invalid_output_anim_sequence_path");
			return false;
		}

		FString SkeletonPath;
		FString PreviewMeshPath;
		JsonTryGetString(Ctx.Params, TEXT("target_skeleton"), SkeletonPath);
		JsonTryGetString(Ctx.Params, TEXT("preview_skeletal_mesh"), PreviewMeshPath);
		USkeletalMesh* PreviewMesh = PreviewMeshPath.TrimStartAndEnd().IsEmpty() ? BindingInfo.SkeletalMesh : UeAgentControlRigOps::LoadAsset<USkeletalMesh>(PreviewMeshPath);
		if (!PreviewMeshPath.TrimStartAndEnd().IsEmpty() && !PreviewMesh)
		{
			OutError = TEXT("preview_skeletal_mesh_not_found");
			return false;
		}
		USkeleton* Skeleton = SkeletonPath.TrimStartAndEnd().IsEmpty() ? nullptr : UeAgentControlRigOps::LoadAsset<USkeleton>(SkeletonPath);
		if (!SkeletonPath.TrimStartAndEnd().IsEmpty() && !Skeleton)
		{
			OutError = TEXT("target_skeleton_not_found");
			return false;
		}
		if (!Skeleton && PreviewMesh)
		{
			Skeleton = PreviewMesh->GetSkeleton();
		}
		if (!Skeleton)
		{
			Skeleton = BindingInfo.Skeleton;
		}
		if (!Skeleton)
		{
			OutError = TEXT("target_skeleton_required_for_new_anim_sequence");
			return false;
		}
		if (BindingInfo.Skeleton && !Skeleton->IsCompatibleForEditor(BindingInfo.Skeleton))
		{
			OutError = TEXT("target_skeleton_not_compatible_with_sequence_binding");
			return false;
		}
		if (PreviewMesh && PreviewMesh->GetSkeleton() && !Skeleton->IsCompatibleForEditor(PreviewMesh->GetSkeleton()))
		{
			OutError = TEXT("preview_skeletal_mesh_not_compatible_with_target_skeleton");
			return false;
		}

		FString CreateError;
		AnimSequence = CreateAnimSequenceAssetForBake(OutputAssetPath, Skeleton, PreviewMesh, CreateError);
		if (!AnimSequence)
		{
			OutError = CreateError.IsEmpty() ? TEXT("create_anim_sequence_failed") : CreateError;
			return false;
		}
		bCreated = true;
	}

	bool bCreateLink = true;
	JsonTryGetBool(Ctx.Params, TEXT("create_link"), bCreateLink);
	UAnimSeqExportOption* ExportOptions = NewObject<UAnimSeqExportOption>(GetTransientPackage(), NAME_None, RF_Transient);
	const FMovieSceneBindingProxy Binding(BindingGuid, Sequence);
	const bool bExported = USequencerToolsFunctionLibrary::ExportAnimSequence(World, Sequence, AnimSequence, ExportOptions, Binding, bCreateLink);
	if (bExported)
	{
		AnimSequence->MarkPackageDirty();
	}

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("sequence_path"), Sequence->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("output_anim_sequence"), AnimSequence->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("binding_id"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphens));
	OutData->SetStringField(TEXT("export_api"), TEXT("USequencerToolsFunctionLibrary::ExportAnimSequence"));
	OutData->SetObjectField(TEXT("binding_preflight"), MakeBakeBindingInfoJson(BindingInfo));
	OutData->SetBoolField(TEXT("created"), bCreated);
	OutData->SetBoolField(TEXT("exported"), bExported);
	OutData->SetStringField(TEXT("status"), TEXT("external_sequence_action"));
	if (!bExported)
	{
		OutError = TEXT("control_rig_bake_to_animation_failed");
	}
	return bExported;
}

bool FUeAgentHttpServer::CmdControlRigBakeToControlRig(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString SequencePath;
	FString ControlRigClassPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("sequence_path"), SequencePath) || SequencePath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("sequence_path_required");
		return false;
	}
	if (!JsonTryGetString(Ctx.Params, TEXT("control_rig_class"), ControlRigClassPath) || ControlRigClassPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("control_rig_class_required");
		return false;
	}
	FGuid BindingGuid;
	if (!ParseGuidParam(Ctx.Params, TEXT("binding_id"), BindingGuid) && !ParseGuidParam(Ctx.Params, TEXT("binding_guid"), BindingGuid))
	{
		OutError = TEXT("binding_id_required");
		return false;
	}

	ULevelSequence* Sequence = UeAgentControlRigOps::LoadAsset<ULevelSequence>(SequencePath);
	if (!Sequence)
	{
		OutError = TEXT("level_sequence_not_found");
		return false;
	}
	UClass* ControlRigClass = LoadObject<UClass>(nullptr, *ControlRigClassPath);
	if (!ControlRigClass || !ControlRigClass->IsChildOf(UControlRig::StaticClass()))
	{
		OutError = TEXT("invalid_control_rig_class");
		return false;
	}

	FString WorldError;
	UWorld* World = GetEditorWorld(WorldError);
	if (!World)
	{
		OutError = WorldError.IsEmpty() ? TEXT("editor_world_not_found") : WorldError;
		return false;
	}

	const FUeAgentControlRigBakeBindingInfo BindingInfo = InspectBakeBinding(World, Sequence, BindingGuid);
	if (!BindingInfo.Error.IsEmpty())
	{
		OutData = MakeShared<FJsonObject>();
		OutData->SetStringField(TEXT("sequence_path"), Sequence->GetOutermost()->GetName());
		OutData->SetStringField(TEXT("control_rig_class"), ControlRigClass->GetPathName());
		OutData->SetStringField(TEXT("binding_id"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphens));
		OutData->SetObjectField(TEXT("binding_preflight"), MakeBakeBindingInfoJson(BindingInfo));
		OutError = BindingInfo.Error;
		return false;
	}

	const bool bIsFKControlRig = ControlRigClass->IsChildOf(UFKControlRig::StaticClass());
	const UControlRig* DefaultControlRig = ControlRigClass->GetDefaultObject<UControlRig>();
	const bool bSupportsInverseEvent = DefaultControlRig &&
		(DefaultControlRig->SupportsEvent(FRigUnit_InverseExecution::EventName) || DefaultControlRig->SupportsEvent(FName(TEXT("Inverse"))));
	if (!bIsFKControlRig && !bSupportsInverseEvent)
	{
		OutData = MakeShared<FJsonObject>();
		OutData->SetStringField(TEXT("sequence_path"), Sequence->GetOutermost()->GetName());
		OutData->SetStringField(TEXT("control_rig_class"), ControlRigClass->GetPathName());
		OutData->SetStringField(TEXT("binding_id"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphens));
		OutData->SetBoolField(TEXT("is_fk_control_rig"), bIsFKControlRig);
		OutData->SetBoolField(TEXT("supports_inverse_event"), bSupportsInverseEvent);
		OutData->SetObjectField(TEXT("binding_preflight"), MakeBakeBindingInfoJson(BindingInfo));
		OutError = TEXT("control_rig_bake_requires_fk_or_inverse_event");
		return false;
	}

	bool bReduceKeys = false;
	JsonTryGetBool(Ctx.Params, TEXT("reduce_keys"), bReduceKeys);
	double ToleranceNum = 0.001;
	JsonTryGetNumber(Ctx.Params, TEXT("tolerance"), ToleranceNum);
	bool bResetControls = true;
	JsonTryGetBool(Ctx.Params, TEXT("reset_controls"), bResetControls);

	UAnimSeqExportOption* ExportOptions = NewObject<UAnimSeqExportOption>(GetTransientPackage(), NAME_None, RF_Transient);
	const FMovieSceneBindingProxy Binding(BindingGuid, Sequence);
	const bool bBaked = UControlRigSequencerEditorLibrary::BakeToControlRig(World, Sequence, ControlRigClass, ExportOptions, bReduceKeys, static_cast<float>(ToleranceNum), Binding, bResetControls);

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("sequence_path"), Sequence->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("control_rig_class"), ControlRigClass->GetPathName());
	OutData->SetStringField(TEXT("binding_id"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphens));
	OutData->SetBoolField(TEXT("is_fk_control_rig"), bIsFKControlRig);
	OutData->SetBoolField(TEXT("supports_inverse_event"), bSupportsInverseEvent);
	OutData->SetObjectField(TEXT("binding_preflight"), MakeBakeBindingInfoJson(BindingInfo));
	OutData->SetBoolField(TEXT("baked"), bBaked);
	if (!bBaked)
	{
		OutError = TEXT("control_rig_bake_to_control_rig_failed");
	}
	return bBaked;
}

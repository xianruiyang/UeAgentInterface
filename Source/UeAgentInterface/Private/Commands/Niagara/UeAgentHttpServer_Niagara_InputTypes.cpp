#include "UeAgentHttpServer_NiagaraOps.h"

#include "Dom/JsonObject.h"
#include "NiagaraTypes.h"

namespace UeAgentNiagaraOps
{
	static FString NormalizeNiagaraInputTypeToken(const FString& InTypeText)
	{
		FString Token = InTypeText;
		Token.TrimStartAndEndInline();
		Token.ReplaceInline(TEXT(" "), TEXT(""));
		Token.ReplaceInline(TEXT("_"), TEXT(""));
		Token.ReplaceInline(TEXT("-"), TEXT(""));
		Token.ReplaceInline(TEXT("'"), TEXT(""));
		Token.ToLowerInline();
		return Token;
	}

	bool IsUserSettableNiagaraEnumValue(const UEnum& EnumType, const int64 EnumValue)
	{
		if (!EnumType.IsValidEnumValue(EnumValue))
		{
			return false;
		}

		const int32 EnumIndex = EnumType.GetIndexByValue(EnumValue);
		if (EnumIndex == INDEX_NONE)
		{
			return false;
		}
		if (EnumType.HasMetaData(TEXT("Hidden"), EnumIndex) ||
			EnumType.HasMetaData(TEXT("HiddenByDefault"), EnumIndex) ||
			EnumType.HasMetaData(TEXT("Spacer"), EnumIndex))
		{
			return false;
		}

		const FString EnumName = EnumType.GetNameStringByValue(EnumValue);
		const FString EnumNameUpper = EnumName.ToUpper();
		if (EnumNameUpper.Equals(TEXT("MAX")) ||
			EnumNameUpper.EndsWith(TEXT("_MAX")) ||
			EnumNameUpper.Contains(TEXT("INVALID")))
		{
			return false;
		}

		const FString FullEnumName = EnumType.GetNameByValue(EnumValue).ToString().ToUpper();
		if (FullEnumName.EndsWith(TEXT("_MAX")))
		{
			return false;
		}
		return true;
	}

	struct FNiagaraInputTypeDescriptor
	{
		FString Family = TEXT("import_text");
		FString ValueType = TEXT("import_text");
		FString ValueSchema = TEXT("import_text");
		FString WritePolicy = TEXT("generic_pin_default_import_text");
		bool bPinDefaultSupported = true;
		bool bPinDefaultValidated = false;
		bool bRequiresEnumOptions = false;
		bool bRequiresSpaceContract = false;
		TArray<FString> ComponentNames;
	};

	static void ConfigureNiagaraComponentDescriptor(
		FNiagaraInputTypeDescriptor& Descriptor,
		const FString& Family,
		const FString& ValueType,
		const TArray<FString>& ComponentNames,
		const bool bRequiresSpaceContract)
	{
		Descriptor.Family = Family;
		Descriptor.ValueType = ValueType;
		Descriptor.ValueSchema = TEXT("components");
		Descriptor.WritePolicy = TEXT("pin_default");
		Descriptor.bPinDefaultSupported = true;
		Descriptor.bPinDefaultValidated = true;
		Descriptor.bRequiresSpaceContract = bRequiresSpaceContract;
		Descriptor.ComponentNames = ComponentNames;
	}

	static FNiagaraInputTypeDescriptor DescribeNiagaraInputType(
		const FString& InTypeText,
		const FNiagaraTypeDefinition& TypeDefinition)
	{
		const FString Token = NormalizeNiagaraInputTypeToken(InTypeText);
		FNiagaraInputTypeDescriptor Descriptor;

		if (Token == TEXT("staticbool") || Token == TEXT("静态布尔"))
		{
			Descriptor.Family = TEXT("static_bool");
			Descriptor.ValueType = TEXT("bool");
			Descriptor.ValueSchema = TEXT("literal");
			Descriptor.WritePolicy = TEXT("static_switch_pin_default");
			Descriptor.bPinDefaultSupported = true;
			Descriptor.bPinDefaultValidated = true;
			return Descriptor;
		}
		if (Token == TEXT("curveforfloats") || Token == TEXT("浮点曲线"))
		{
			Descriptor.Family = TEXT("float_curve");
			Descriptor.ValueType = TEXT("float_curve");
			Descriptor.ValueSchema = TEXT("curve_json");
			Descriptor.WritePolicy = TEXT("dynamic_input_or_data_interface_curve");
			Descriptor.bPinDefaultSupported = false;
			return Descriptor;
		}
		if (Token == TEXT("curveforcolors") || Token == TEXT("颜色曲线"))
		{
			Descriptor.Family = TEXT("color_curve");
			Descriptor.ValueType = TEXT("color_curve");
			Descriptor.ValueSchema = TEXT("curve_json");
			Descriptor.WritePolicy = TEXT("dynamic_input_or_data_interface_curve");
			Descriptor.bPinDefaultSupported = false;
			return Descriptor;
		}
		if (Token == TEXT("curveforvector2ds") || Token == TEXT("curveforvector2d"))
		{
			Descriptor.Family = TEXT("vector2_curve");
			Descriptor.ValueType = TEXT("vector2_curve");
			Descriptor.ValueSchema = TEXT("curve_json");
			Descriptor.WritePolicy = TEXT("dynamic_input_or_data_interface_curve");
			Descriptor.bPinDefaultSupported = false;
			return Descriptor;
		}
		if (Token == TEXT("matrix") || Token == TEXT("矩阵"))
		{
			Descriptor.Family = TEXT("matrix");
			Descriptor.ValueType = TEXT("matrix");
			Descriptor.ValueSchema = TEXT("struct_import_text");
			Descriptor.WritePolicy = TEXT("generic_pin_default_import_text");
			return Descriptor;
		}
		if (Token.Contains(TEXT("rendererinfo")) || Token.Contains(TEXT("渲染器信息")))
		{
			Descriptor.Family = TEXT("renderer_info_struct");
			Descriptor.ValueType = TEXT("renderer_info");
			Descriptor.ValueSchema = TEXT("struct_import_text");
			Descriptor.WritePolicy = TEXT("renderer_or_struct_schema");
			Descriptor.bPinDefaultSupported = false;
			return Descriptor;
		}

		if (TypeDefinition.IsEnum())
		{
			Descriptor.Family = TEXT("enum");
			Descriptor.ValueType = TEXT("enum");
			Descriptor.ValueSchema = TEXT("enum");
			Descriptor.WritePolicy = TEXT("pin_default");
			Descriptor.bPinDefaultSupported = true;
			Descriptor.bPinDefaultValidated = true;
			Descriptor.bRequiresEnumOptions = true;
			return Descriptor;
		}
		if (TypeDefinition.IsDataInterface())
		{
			Descriptor.Family = TEXT("data_interface");
			Descriptor.ValueType = TEXT("data_interface");
			Descriptor.ValueSchema = TEXT("object_path");
			Descriptor.WritePolicy = TEXT("data_interface_reference");
			Descriptor.bPinDefaultSupported = false;
			return Descriptor;
		}
		if (TypeDefinition.IsUObject())
		{
			Descriptor.Family = TEXT("uobject");
			Descriptor.ValueType = TEXT("soft_object_path");
			Descriptor.ValueSchema = TEXT("object_path");
			Descriptor.WritePolicy = TEXT("uobject_reference");
			Descriptor.bPinDefaultSupported = false;
			return Descriptor;
		}
		if (TypeDefinition == FNiagaraTypeDefinition::GetBoolDef())
		{
			Descriptor.Family = TEXT("bool");
			Descriptor.ValueType = TEXT("bool");
			Descriptor.ValueSchema = TEXT("literal");
			Descriptor.WritePolicy = TEXT("pin_default");
			Descriptor.bPinDefaultSupported = true;
			Descriptor.bPinDefaultValidated = true;
			return Descriptor;
		}
		if (TypeDefinition == FNiagaraTypeDefinition::GetIntDef())
		{
			Descriptor.Family = TEXT("int");
			Descriptor.ValueType = TEXT("int");
			Descriptor.ValueSchema = TEXT("literal");
			Descriptor.WritePolicy = TEXT("pin_default");
			Descriptor.bPinDefaultSupported = true;
			Descriptor.bPinDefaultValidated = true;
			return Descriptor;
		}
		if (TypeDefinition == FNiagaraTypeDefinition::GetFloatDef())
		{
			Descriptor.Family = TEXT("float");
			Descriptor.ValueType = TEXT("float");
			Descriptor.ValueSchema = TEXT("literal");
			Descriptor.WritePolicy = TEXT("pin_default");
			Descriptor.bPinDefaultSupported = true;
			Descriptor.bPinDefaultValidated = true;
			return Descriptor;
		}
		if (TypeDefinition == FNiagaraTypeHelper::GetDoubleDef())
		{
			Descriptor.Family = TEXT("double");
			Descriptor.ValueType = TEXT("double");
			Descriptor.ValueSchema = TEXT("literal");
			Descriptor.WritePolicy = TEXT("pin_default");
			Descriptor.bPinDefaultSupported = true;
			Descriptor.bPinDefaultValidated = true;
			return Descriptor;
		}
		if (TypeDefinition == FNiagaraTypeDefinition::GetVec2Def())
		{
			ConfigureNiagaraComponentDescriptor(Descriptor, TEXT("vector2"), TEXT("vector2"), { TEXT("X"), TEXT("Y") }, false);
			return Descriptor;
		}
		if (TypeDefinition == FNiagaraTypeDefinition::GetVec3Def())
		{
			ConfigureNiagaraComponentDescriptor(Descriptor, TEXT("vector3"), TEXT("vector3"), { TEXT("X"), TEXT("Y"), TEXT("Z") }, false);
			return Descriptor;
		}
		if (TypeDefinition == FNiagaraTypeDefinition::GetPositionDef())
		{
			ConfigureNiagaraComponentDescriptor(Descriptor, TEXT("position"), TEXT("position"), { TEXT("X"), TEXT("Y"), TEXT("Z") }, true);
			return Descriptor;
		}
		if (TypeDefinition == FNiagaraTypeDefinition::GetVec4Def())
		{
			ConfigureNiagaraComponentDescriptor(Descriptor, TEXT("vector4"), TEXT("vector4"), { TEXT("X"), TEXT("Y"), TEXT("Z"), TEXT("W") }, false);
			return Descriptor;
		}
		if (TypeDefinition == FNiagaraTypeDefinition::GetQuatDef())
		{
			ConfigureNiagaraComponentDescriptor(Descriptor, TEXT("quat"), TEXT("quat"), { TEXT("X"), TEXT("Y"), TEXT("Z"), TEXT("W") }, false);
			return Descriptor;
		}
		if (TypeDefinition == FNiagaraTypeDefinition::GetColorDef())
		{
			ConfigureNiagaraComponentDescriptor(Descriptor, TEXT("linear_color"), TEXT("linear_color"), { TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A") }, false);
			return Descriptor;
		}
		if (TypeDefinition.GetStruct())
		{
			Descriptor.Family = TEXT("generic_struct");
			Descriptor.ValueType = TEXT("struct_import_text");
			Descriptor.ValueSchema = TEXT("struct_import_text");
			Descriptor.WritePolicy = TEXT("generic_pin_default_import_text");
			return Descriptor;
		}

		if (Token.StartsWith(TEXT("eniagara")) || Token.StartsWith(TEXT("e")))
		{
			Descriptor.Family = TEXT("enum");
			Descriptor.ValueType = TEXT("enum");
			Descriptor.ValueSchema = TEXT("enum");
			Descriptor.WritePolicy = TEXT("pin_default");
			Descriptor.bPinDefaultSupported = true;
			Descriptor.bPinDefaultValidated = true;
			Descriptor.bRequiresEnumOptions = true;
			return Descriptor;
		}
		return Descriptor;
	}

	TSharedPtr<FJsonObject> BuildNiagaraInputTypeDescriptorJson(
		const FString& InTypeText,
		const FNiagaraTypeDefinition& TypeDefinition)
	{
		const FNiagaraInputTypeDescriptor Descriptor = DescribeNiagaraInputType(InTypeText, TypeDefinition);
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("family"), Descriptor.Family);
		Obj->SetStringField(TEXT("value_type"), Descriptor.ValueType);
		Obj->SetStringField(TEXT("value_schema"), Descriptor.ValueSchema);
		Obj->SetStringField(TEXT("write_policy"), Descriptor.WritePolicy);
		Obj->SetBoolField(TEXT("pin_default_supported"), Descriptor.bPinDefaultSupported);
		Obj->SetBoolField(TEXT("pin_default_validated"), Descriptor.bPinDefaultValidated);
		Obj->SetBoolField(TEXT("requires_enum_options"), Descriptor.bRequiresEnumOptions);
		Obj->SetBoolField(TEXT("requires_space_contract"), Descriptor.bRequiresSpaceContract);
		if (Descriptor.ComponentNames.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> ComponentNamesJson;
			ComponentNamesJson.Reserve(Descriptor.ComponentNames.Num());
			for (const FString& ComponentName : Descriptor.ComponentNames)
			{
				ComponentNamesJson.Add(MakeShared<FJsonValueString>(ComponentName));
			}
			Obj->SetArrayField(TEXT("component_names"), ComponentNamesJson);
		}
		return Obj;
	}
}

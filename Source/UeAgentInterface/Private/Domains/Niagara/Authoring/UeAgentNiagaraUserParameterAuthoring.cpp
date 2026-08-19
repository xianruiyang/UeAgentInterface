// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Niagara/Authoring/UeAgentNiagaraUserParameterAuthoring.h"

#include "Domains/Niagara/Authoring/UeAgentNiagaraStageModuleAuthoring.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "NiagaraDataInterface.h"
#include "NiagaraParameterStore.h"
#include "NiagaraSystem.h"
#include "Utilities/UeAgentPropertyValueCodec.h"

namespace UeAgentNiagaraUserParameterAuthoring
{
	namespace
	{
		FString NormalizeNameForComparison(const FString& Name)
		{
			FString Result = CanonicalizeName(Name);
			Result.ToLowerInline();
			return Result;
		}

		bool ResolveStoreVariable(
			const FNiagaraParameterStore& Store,
			const FNiagaraVariable& Variable,
			FNiagaraVariable& OutVariable)
		{
			if (Store.IndexOf(Variable) != INDEX_NONE)
			{
				OutVariable = Variable;
				return true;
			}
			const FString SearchName = NormalizeNameForComparison(Variable.GetName().ToString());
			for (const FNiagaraVariableWithOffset& Stored : Store.ReadParameterVariables())
			{
				if (Stored.GetType() == Variable.GetType()
					&& NormalizeNameForComparison(Stored.GetName().ToString()) == SearchName)
				{
					OutVariable = FNiagaraVariable(Stored.GetType(), Stored.GetName());
					return true;
				}
			}
			return false;
		}

		bool ReadLiteralBool(const TSharedPtr<FJsonValue>& Value, bool& OutValue)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			return Value.IsValid()
				&& Value->TryGetObject(Object)
				&& Object && Object->IsValid()
				&& (*Object)->TryGetBoolField(TEXT("literal"), OutValue);
		}

		bool ReadLiteralNumber(const TSharedPtr<FJsonValue>& Value, double& OutValue)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			return Value.IsValid()
				&& Value->TryGetObject(Object)
				&& Object && Object->IsValid()
				&& (*Object)->TryGetNumberField(TEXT("literal"), OutValue)
				&& FMath::IsFinite(OutValue);
		}

		bool ReadComponents(
			const TSharedPtr<FJsonValue>& Value,
			const int32 ExpectedCount,
			TArray<double>& OutComponents)
		{
			OutComponents.Reset();
			const TSharedPtr<FJsonObject>* Object = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
			if (!Value.IsValid()
				|| !Value->TryGetObject(Object)
				|| !Object || !Object->IsValid()
				|| !(*Object)->TryGetArrayField(TEXT("components"), Items)
				|| !Items || Items->Num() != ExpectedCount)
			{
				return false;
			}
			OutComponents.Reserve(ExpectedCount);
			for (const TSharedPtr<FJsonValue>& Item : *Items)
			{
				double Number = 0.0;
				if (!Item.IsValid() || !Item->TryGetNumber(Number) || !FMath::IsFinite(Number))
				{
					OutComponents.Reset();
					return false;
				}
				OutComponents.Add(Number);
			}
			return true;
		}

		TSharedRef<FJsonObject> Literal(const bool Value)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetBoolField(TEXT("literal"), Value);
			return Result;
		}

		TSharedRef<FJsonObject> Literal(const double Value)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetNumberField(TEXT("literal"), Value);
			return Result;
		}

		TSharedRef<FJsonObject> Components(const TArray<double>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> Items;
			Items.Reserve(Values.Num());
			for (const double Value : Values) Items.Add(MakeShared<FJsonValueNumber>(Value));
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetArrayField(TEXT("components"), Items);
			return Result;
		}

		FString GetValueKind(const FNiagaraTypeDefinition& Type)
		{
			if (Type == FNiagaraTypeDefinition::GetBoolDef()) return TEXT("bool");
			if (Type == FNiagaraTypeDefinition::GetIntDef()) return TEXT("int32");
			if (Type == FNiagaraTypeDefinition::GetFloatDef()) return TEXT("float");
			if (Type == FNiagaraTypeHelper::GetDoubleDef()) return TEXT("double");
			if (Type == FNiagaraTypeDefinition::GetVec2Def()) return TEXT("vector2");
			if (Type == FNiagaraTypeDefinition::GetVec3Def()) return TEXT("vector3");
			if (Type == FNiagaraTypeDefinition::GetPositionDef()) return TEXT("position");
			if (Type == FNiagaraTypeDefinition::GetVec4Def()) return TEXT("vector4");
			if (Type == FNiagaraTypeDefinition::GetQuatDef()) return TEXT("quat");
			if (Type == FNiagaraTypeDefinition::GetColorDef()) return TEXT("linear_color");
			if (Type.IsEnum()) return TEXT("enum");
			if (Type.IsDataInterface() || Type.IsUObject()) return TEXT("object_ref");
			return TEXT("struct");
		}

		bool DecodeValue(
			const FNiagaraTypeDefinition& Type,
			const TSharedPtr<FJsonValue>& Value,
			TArray<uint8>& OutBytes,
			UObject*& OutObject,
			FString& OutError)
		{
			OutBytes.Reset();
			OutObject = nullptr;
			if (Type.IsDataInterface() || Type.IsUObject())
			{
				const TSharedPtr<FJsonObject>* Object = nullptr;
				FString Path;
				if (!Value.IsValid() || !Value->TryGetObject(Object) || !Object || !Object->IsValid()
					|| !(*Object)->TryGetStringField(TEXT("path"), Path))
				{
					OutError = TEXT("niagara_user_parameter_object_value_invalid");
					return false;
				}
				Path.TrimStartAndEndInline();
				if (!Path.IsEmpty() && !Path.Equals(TEXT("None"), ESearchCase::IgnoreCase))
				{
					OutObject = StaticLoadObject(UObject::StaticClass(), nullptr, *Path);
					if (!OutObject)
					{
						OutError = TEXT("niagara_user_parameter_object_not_found:") + Path;
						return false;
					}
				}
				if (Type.IsDataInterface() && OutObject && !Cast<UNiagaraDataInterface>(OutObject))
				{
					OutError = TEXT("niagara_user_parameter_object_type_mismatch");
					return false;
				}
				if (Type.IsUObject() && OutObject && Type.GetClass() && !OutObject->IsA(Type.GetClass()))
				{
					OutError = TEXT("niagara_user_parameter_object_type_mismatch");
					return false;
				}
				return true;
			}

			OutBytes.SetNumZeroed(Type.GetSize());
			if (Type == FNiagaraTypeDefinition::GetBoolDef())
			{
				bool bValue = false;
				if (!ReadLiteralBool(Value, bValue))
				{
					OutError = TEXT("niagara_user_parameter_bool_value_invalid");
					return false;
				}
				FNiagaraBool NiagaraValue;
				NiagaraValue.SetValue(bValue);
				FMemory::Memcpy(OutBytes.GetData(), &NiagaraValue, sizeof(NiagaraValue));
				return true;
			}
			if (Type == FNiagaraTypeDefinition::GetIntDef())
			{
				double Number = 0.0;
				if (!ReadLiteralNumber(Value, Number)
					|| Number < static_cast<double>(MIN_int32)
					|| Number > static_cast<double>(MAX_int32)
					|| !FMath::IsNearlyEqual(Number, FMath::RoundToDouble(Number)))
				{
					OutError = TEXT("niagara_user_parameter_int_value_invalid");
					return false;
				}
				const int32 IntValue = static_cast<int32>(Number);
				FMemory::Memcpy(OutBytes.GetData(), &IntValue, sizeof(IntValue));
				return true;
			}
			if (Type == FNiagaraTypeDefinition::GetFloatDef() || Type == FNiagaraTypeHelper::GetDoubleDef())
			{
				double Number = 0.0;
				if (!ReadLiteralNumber(Value, Number))
				{
					OutError = TEXT("niagara_user_parameter_number_value_invalid");
					return false;
				}
				if (Type == FNiagaraTypeDefinition::GetFloatDef())
				{
					const float FloatValue = static_cast<float>(Number);
					if (!FMath::IsFinite(FloatValue))
					{
						OutError = TEXT("niagara_user_parameter_number_value_invalid");
						return false;
					}
					FMemory::Memcpy(OutBytes.GetData(), &FloatValue, sizeof(FloatValue));
				}
				else
				{
					FMemory::Memcpy(OutBytes.GetData(), &Number, sizeof(Number));
				}
				return true;
			}

			int32 ComponentCount = 0;
			if (Type == FNiagaraTypeDefinition::GetVec2Def()) ComponentCount = 2;
			else if (Type == FNiagaraTypeDefinition::GetVec3Def() || Type == FNiagaraTypeDefinition::GetPositionDef()) ComponentCount = 3;
			else if (Type == FNiagaraTypeDefinition::GetVec4Def()
				|| Type == FNiagaraTypeDefinition::GetQuatDef()
				|| Type == FNiagaraTypeDefinition::GetColorDef()) ComponentCount = 4;
			if (ComponentCount > 0)
			{
				TArray<double> Values;
				if (!ReadComponents(Value, ComponentCount, Values))
				{
					OutError = TEXT("niagara_user_parameter_components_invalid");
					return false;
				}
				if (Type == FNiagaraTypeDefinition::GetVec2Def())
				{
					const FVector2f Typed(static_cast<float>(Values[0]), static_cast<float>(Values[1]));
					FMemory::Memcpy(OutBytes.GetData(), &Typed, sizeof(Typed));
				}
				else if (Type == FNiagaraTypeDefinition::GetVec3Def())
				{
					const FVector3f Typed(static_cast<float>(Values[0]), static_cast<float>(Values[1]), static_cast<float>(Values[2]));
					FMemory::Memcpy(OutBytes.GetData(), &Typed, sizeof(Typed));
				}
				else if (Type == FNiagaraTypeDefinition::GetPositionDef())
				{
					const FNiagaraPosition Typed(static_cast<float>(Values[0]), static_cast<float>(Values[1]), static_cast<float>(Values[2]));
					FMemory::Memcpy(OutBytes.GetData(), &Typed, sizeof(Typed));
				}
				else if (Type == FNiagaraTypeDefinition::GetVec4Def())
				{
					const FVector4f Typed(static_cast<float>(Values[0]), static_cast<float>(Values[1]), static_cast<float>(Values[2]), static_cast<float>(Values[3]));
					FMemory::Memcpy(OutBytes.GetData(), &Typed, sizeof(Typed));
				}
				else if (Type == FNiagaraTypeDefinition::GetQuatDef())
				{
					const FQuat4f Typed(static_cast<float>(Values[0]), static_cast<float>(Values[1]), static_cast<float>(Values[2]), static_cast<float>(Values[3]));
					FMemory::Memcpy(OutBytes.GetData(), &Typed, sizeof(Typed));
				}
				else
				{
					const FLinearColor Typed(Values[0], Values[1], Values[2], Values[3]);
					FMemory::Memcpy(OutBytes.GetData(), &Typed, sizeof(Typed));
				}
				return true;
			}

			if (Type.IsEnum())
			{
				const TSharedPtr<FJsonObject>* Object = nullptr;
				FString Selected;
				UEnum* Enum = Type.GetEnum();
				if (!Enum || !Value.IsValid() || !Value->TryGetObject(Object) || !Object || !Object->IsValid()
					|| !(*Object)->TryGetStringField(TEXT("selected"), Selected))
				{
					OutError = TEXT("niagara_user_parameter_enum_value_invalid");
					return false;
				}
				int64 EnumValue = Enum->GetValueByNameString(Selected, EGetByNameFlags::CheckAuthoredName);
				if (EnumValue == INDEX_NONE) EnumValue = Enum->GetValueByNameString(Selected, EGetByNameFlags::None);
				if (EnumValue == INDEX_NONE || !Enum->IsValidEnumValue(EnumValue))
				{
					OutError = TEXT("niagara_user_parameter_enum_value_invalid");
					return false;
				}
				const int32 Stored = static_cast<int32>(EnumValue);
				FMemory::Memcpy(OutBytes.GetData(), &Stored, sizeof(Stored));
				return true;
			}

			UScriptStruct* Struct = Cast<UScriptStruct>(Type.GetStruct());
			if (!Struct)
			{
				OutError = TEXT("niagara_user_parameter_type_not_structured");
				return false;
			}
			Struct->InitializeStruct(OutBytes.GetData());
			const bool bDecoded = UeAgentPropertyValueCodec::DecodeStructValue(
				Struct, Value, OutBytes.GetData(), OutError);
			if (!bDecoded) Struct->DestroyStruct(OutBytes.GetData());
			return bDecoded;
		}

		bool SetValue(
			FNiagaraParameterStore& Store,
			const FNiagaraVariable& Variable,
			const TSharedPtr<FJsonValue>& Value,
			FString& OutError)
		{
			TArray<uint8> Bytes;
			UObject* ObjectValue = nullptr;
			if (!DecodeValue(Variable.GetType(), Value, Bytes, ObjectValue, OutError)) return false;
			if (Variable.GetType().IsDataInterface())
			{
				Store.SetDataInterface(Cast<UNiagaraDataInterface>(ObjectValue), Variable);
				return true;
			}
			if (Variable.GetType().IsUObject())
			{
				Store.SetUObject(ObjectValue, Variable);
				return true;
			}
			const bool bSet = Store.SetParameterData(Bytes.GetData(), Variable, false);
			if (UScriptStruct* Struct = Cast<UScriptStruct>(Variable.GetType().GetStruct()))
			{
				if (GetValueKind(Variable.GetType()) == TEXT("struct")) Struct->DestroyStruct(Bytes.GetData());
			}
			if (!bSet) OutError = TEXT("niagara_user_parameter_set_data_failed");
			return bSet;
		}

		TSharedPtr<FJsonValue> EncodeValue(
			const FNiagaraParameterStore& Store,
			const FNiagaraVariable& Variable,
			bool& bOutHasValue,
			FString& OutValueText)
		{
			bOutHasValue = false;
			OutValueText.Reset();
			const FNiagaraTypeDefinition& Type = Variable.GetType();
			if (Type.IsDataInterface() || Type.IsUObject())
			{
				UObject* ObjectValue = Type.IsDataInterface()
					? Cast<UObject>(Store.GetDataInterface(Variable))
					: Store.GetUObject(Variable).Get();
				TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
				Object->SetStringField(TEXT("path"), ObjectValue ? ObjectValue->GetPathName() : TEXT("None"));
				bOutHasValue = true;
				OutValueText = ObjectValue ? ObjectValue->GetPathName() : TEXT("None");
				return MakeShared<FJsonValueObject>(Object);
			}

			const uint8* Data = Store.GetParameterData(Variable);
			if (!Data) return nullptr;
			bOutHasValue = true;
			OutValueText = Type.ToString(Data);
			if (Type == FNiagaraTypeDefinition::GetBoolDef())
			{
				return MakeShared<FJsonValueObject>(Literal(reinterpret_cast<const FNiagaraBool*>(Data)->GetValue()));
			}
			if (Type == FNiagaraTypeDefinition::GetIntDef())
			{
				return MakeShared<FJsonValueObject>(Literal(static_cast<double>(*reinterpret_cast<const int32*>(Data))));
			}
			if (Type == FNiagaraTypeDefinition::GetFloatDef())
			{
				return MakeShared<FJsonValueObject>(Literal(*reinterpret_cast<const float*>(Data)));
			}
			if (Type == FNiagaraTypeHelper::GetDoubleDef())
			{
				return MakeShared<FJsonValueObject>(Literal(*reinterpret_cast<const double*>(Data)));
			}
			if (Type == FNiagaraTypeDefinition::GetVec2Def())
			{
				const FVector2f& Typed = *reinterpret_cast<const FVector2f*>(Data);
				return MakeShared<FJsonValueObject>(Components({ Typed.X, Typed.Y }));
			}
			if (Type == FNiagaraTypeDefinition::GetVec3Def())
			{
				const FVector3f& Typed = *reinterpret_cast<const FVector3f*>(Data);
				return MakeShared<FJsonValueObject>(Components({ Typed.X, Typed.Y, Typed.Z }));
			}
			if (Type == FNiagaraTypeDefinition::GetPositionDef())
			{
				const FNiagaraPosition& Typed = *reinterpret_cast<const FNiagaraPosition*>(Data);
				return MakeShared<FJsonValueObject>(Components({ Typed.X, Typed.Y, Typed.Z }));
			}
			if (Type == FNiagaraTypeDefinition::GetVec4Def())
			{
				const FVector4f& Typed = *reinterpret_cast<const FVector4f*>(Data);
				return MakeShared<FJsonValueObject>(Components({ Typed.X, Typed.Y, Typed.Z, Typed.W }));
			}
			if (Type == FNiagaraTypeDefinition::GetQuatDef())
			{
				const FQuat4f& Typed = *reinterpret_cast<const FQuat4f*>(Data);
				return MakeShared<FJsonValueObject>(Components({ Typed.X, Typed.Y, Typed.Z, Typed.W }));
			}
			if (Type == FNiagaraTypeDefinition::GetColorDef())
			{
				const FLinearColor& Typed = *reinterpret_cast<const FLinearColor*>(Data);
				return MakeShared<FJsonValueObject>(Components({ Typed.R, Typed.G, Typed.B, Typed.A }));
			}
			if (Type.IsEnum() && Type.GetEnum())
			{
				TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
				Object->SetStringField(TEXT("selected"), Type.GetEnum()->GetNameStringByValue(*reinterpret_cast<const int32*>(Data)));
				return MakeShared<FJsonValueObject>(Object);
			}
			UeAgentPropertyValueCodec::FOptions Options;
			UScriptStruct* Struct = Cast<UScriptStruct>(Type.GetStruct());
			return Struct
				? UeAgentPropertyValueCodec::EncodeStructValue(Struct, Data, nullptr, Options)
				: nullptr;
		}
	}

	FString CanonicalizeName(const FString& Name)
	{
		FString Result = Name.TrimStartAndEnd();
		if (!Result.StartsWith(TEXT("User."), ESearchCase::IgnoreCase)) Result = TEXT("User.") + Result;
		return Result;
	}

	void Enumerate(const UNiagaraSystem& System, TArray<FNiagaraVariable>& OutVariables)
	{
		System.GetExposedParameters().GetUserParameters(OutVariables);
		OutVariables.Sort([](const FNiagaraVariable& Left, const FNiagaraVariable& Right)
		{
			return Left.GetName().LexicalLess(Right.GetName());
		});
	}

	bool FindByName(const UNiagaraSystem& System, const FString& ParameterName, FNiagaraVariable& OutVariable)
	{
		const FString Search = NormalizeNameForComparison(ParameterName);
		TArray<FNiagaraVariable> Variables;
		Enumerate(System, Variables);
		for (const FNiagaraVariable& Variable : Variables)
		{
			if (NormalizeNameForComparison(Variable.GetName().ToString()) == Search)
			{
				OutVariable = Variable;
				return true;
			}
		}
		return false;
	}

	bool ParsePatches(
		UNiagaraSystem* System,
		const TSharedPtr<const FJsonObject>& ProviderExtension,
		TArray<FPatch>& OutPatches,
		FString& OutError)
	{
		OutPatches.Reset();
		OutError.Reset();
		if (!ProviderExtension.IsValid() || !ProviderExtension->HasField(TEXT("user_parameters"))) return true;
		if (!System)
		{
			OutError = TEXT("niagara_workspace_user_parameters_require_system");
			return false;
		}
		const TSharedPtr<FJsonObject>* Parameters = nullptr;
		if (!ProviderExtension->TryGetObjectField(TEXT("user_parameters"), Parameters)
			|| !Parameters || !Parameters->IsValid())
		{
			OutError = TEXT("niagara_workspace_user_parameters_invalid");
			return false;
		}
		TSet<FString> SeenNames;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Parameters)->Values)
		{
			FPatch& Patch = OutPatches.AddDefaulted_GetRef();
			Patch.SourceKey = Pair.Key;
			Patch.ParameterName = CanonicalizeName(Pair.Key);
			const FString ComparisonName = NormalizeNameForComparison(Patch.ParameterName);
			if (Patch.ParameterName == TEXT("User.") || SeenNames.Contains(ComparisonName))
			{
				OutError = TEXT("niagara_workspace_user_parameter_identity_invalid:") + Pair.Key;
				return false;
			}
			SeenNames.Add(ComparisonName);
			FNiagaraVariable Existing;
			const bool bExists = FindByName(*System, Patch.ParameterName, Existing);
			if (!Pair.Value.IsValid() || Pair.Value->Type == EJson::Null)
			{
				Patch.bRemove = true;
				continue;
			}
			if (Pair.Value->Type != EJson::Object)
			{
				OutError = TEXT("niagara_workspace_user_parameter_invalid:") + Pair.Key;
				return false;
			}
			const TSharedPtr<FJsonObject> Object = Pair.Value->AsObject();
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Object->Values)
			{
				if (Field.Key != TEXT("type") && Field.Key != TEXT("value")
					&& Field.Key != TEXT("value_kind") && Field.Key != TEXT("remove")
					&& Field.Key != TEXT("has_value") && Field.Key != TEXT("value_text"))
				{
					OutError = TEXT("niagara_workspace_user_parameter_unknown_field:") + Field.Key;
					return false;
				}
			}
			bool bDeclaredHasValue = false;
			const bool bHasDeclaredValueState = Object->HasField(TEXT("has_value"));
			if (bHasDeclaredValueState && !Object->TryGetBoolField(TEXT("has_value"), bDeclaredHasValue))
			{
				OutError = TEXT("niagara_workspace_user_parameter_has_value_invalid:") + Pair.Key;
				return false;
			}
			FString ValueText;
			if (Object->HasField(TEXT("value_text"))
				&& !Object->TryGetStringField(TEXT("value_text"), ValueText))
			{
				OutError = TEXT("niagara_workspace_user_parameter_value_text_invalid:") + Pair.Key;
				return false;
			}
			Object->TryGetBoolField(TEXT("remove"), Patch.bRemove);
			if (Patch.bRemove) continue;
			FString TypeText;
			if (Object->HasField(TEXT("type")) && !Object->TryGetStringField(TEXT("type"), TypeText))
			{
				OutError = TEXT("niagara_workspace_user_parameter_type_invalid:") + Pair.Key;
				return false;
			}
			Patch.bTypeExplicit = !TypeText.TrimStartAndEnd().IsEmpty();
			if (Patch.bTypeExplicit)
			{
				if (!UeAgentNiagaraStageModuleAuthoring::ResolveNiagaraTypeDefinition(
					TypeText, Patch.TypeDefinition, OutError))
				{
					OutError = TEXT("niagara_workspace_user_parameter_type_invalid:") + Pair.Key + TEXT(":") + OutError;
					return false;
				}
			}
			else if (bExists)
			{
				Patch.TypeDefinition = Existing.GetType();
			}
			else
			{
				OutError = TEXT("niagara_workspace_user_parameter_type_required:") + Pair.Key;
				return false;
			}
			if (bExists && Existing.GetType() != Patch.TypeDefinition)
			{
				OutError = TEXT("niagara_workspace_user_parameter_type_conflict:") + Pair.Key;
				return false;
			}
			FString ValueKind;
			if (Object->HasField(TEXT("value_kind")))
			{
				if (!Object->TryGetStringField(TEXT("value_kind"), ValueKind)
					|| !ValueKind.Equals(GetValueKind(Patch.TypeDefinition), ESearchCase::IgnoreCase))
				{
					OutError = TEXT("niagara_workspace_user_parameter_value_kind_conflict:") + Pair.Key;
					return false;
				}
			}
			Patch.Value = Object->TryGetField(TEXT("value"));
			Patch.bHasValue = Patch.Value.IsValid();
			if (bHasDeclaredValueState && !bDeclaredHasValue && Patch.bHasValue)
			{
				OutError = TEXT("niagara_workspace_user_parameter_has_value_conflict:") + Pair.Key;
				return false;
			}
			if (Patch.bHasValue)
			{
				TArray<uint8> ValidationBytes;
				UObject* ValidationObject = nullptr;
				if (!DecodeValue(Patch.TypeDefinition, Patch.Value, ValidationBytes, ValidationObject, OutError))
				{
					OutError = TEXT("niagara_workspace_user_parameter_value_invalid:") + Pair.Key + TEXT(":") + OutError;
					return false;
				}
				if (UScriptStruct* Struct = Cast<UScriptStruct>(Patch.TypeDefinition.GetStruct()))
				{
					if (GetValueKind(Patch.TypeDefinition) == TEXT("struct")) Struct->DestroyStruct(ValidationBytes.GetData());
				}
			}
		}
		return true;
	}

	bool ApplyPatches(UNiagaraSystem& System, const TArray<FPatch>& Patches, FString& OutError)
	{
		OutError.Reset();
		if (Patches.IsEmpty()) return true;
		System.Modify();
		FNiagaraUserRedirectionParameterStore& Store = System.GetExposedParameters();
		for (const FPatch& Patch : Patches)
		{
			FNiagaraVariable Existing;
			const bool bExists = FindByName(System, Patch.ParameterName, Existing);
			if (Patch.bRemove)
			{
				if (bExists && !Store.RemoveParameter(Existing))
				{
					OutError = TEXT("niagara_workspace_user_parameter_remove_failed:") + Patch.ParameterName;
					return false;
				}
				continue;
			}
			FNiagaraVariable Variable = Existing;
			if (!bExists)
			{
				Variable = FNiagaraVariable(Patch.TypeDefinition, FName(*Patch.ParameterName));
				FNiagaraUserRedirectionParameterStore::MakeUserVariable(Variable);
				if (!Store.AddParameter(Variable, true, true))
				{
					OutError = TEXT("niagara_workspace_user_parameter_add_failed:") + Patch.ParameterName;
					return false;
				}
				if (!FindByName(System, Patch.ParameterName, Variable))
				{
					OutError = TEXT("niagara_workspace_user_parameter_missing_after_add:") + Patch.ParameterName;
					return false;
				}
			}
			if (Patch.bHasValue && !SetValue(Store, Variable, Patch.Value, OutError))
			{
				OutError = TEXT("niagara_workspace_user_parameter_set_failed:") + Patch.ParameterName + TEXT(":") + OutError;
				return false;
			}
		}
		return true;
	}

	bool ApplyValuePatchesToStore(
		const UNiagaraSystem& System,
		FNiagaraParameterStore& Store,
		const TArray<FPatch>& Patches,
		FString& OutError)
	{
		OutError.Reset();
		for (const FPatch& Patch : Patches)
		{
			FNiagaraVariable Variable;
			if (!FindByName(System, Patch.ParameterName, Variable))
			{
				OutError = TEXT("niagara_runtime_user_parameter_not_found:") + Patch.ParameterName;
				return false;
			}
			if (Patch.bRemove)
			{
				OutError = TEXT("niagara_runtime_user_parameter_remove_unsupported:") + Patch.ParameterName;
				return false;
			}
			if (Variable.GetType() != Patch.TypeDefinition)
			{
				OutError = TEXT("niagara_runtime_user_parameter_type_conflict:") + Patch.ParameterName;
				return false;
			}
			FNiagaraVariable StoreVariable;
			if (!ResolveStoreVariable(Store, Variable, StoreVariable))
			{
				OutError = TEXT("niagara_runtime_user_parameter_not_in_store:") + Patch.ParameterName;
				return false;
			}
			if (Patch.bHasValue && !SetValue(Store, StoreVariable, Patch.Value, OutError))
			{
				OutError = TEXT("niagara_runtime_user_parameter_set_failed:") + Patch.ParameterName + TEXT(":") + OutError;
				return false;
			}
		}
		return true;
	}

	void CaptureSnapshot(const UNiagaraSystem& System, TArray<FSnapshotEntry>& OutSnapshot)
	{
		OutSnapshot.Reset();
		TArray<FNiagaraVariable> Variables;
		Enumerate(System, Variables);
		const FNiagaraUserRedirectionParameterStore& Store = System.GetExposedParameters();
		for (const FNiagaraVariable& Variable : Variables)
		{
			FSnapshotEntry& Entry = OutSnapshot.AddDefaulted_GetRef();
			Entry.Variable = Variable;
			if (Variable.GetType().IsDataInterface())
			{
				Entry.ObjectValue.Reset(Store.GetDataInterface(Variable));
				Entry.bHasValue = true;
			}
			else if (Variable.GetType().IsUObject())
			{
				Entry.ObjectValue.Reset(Store.GetUObject(Variable));
				Entry.bHasValue = true;
			}
			else if (const uint8* Data = Store.GetParameterData(Variable))
			{
				Entry.ValueBytes.Append(Data, Variable.GetType().GetSize());
				Entry.bHasValue = true;
			}
		}
	}

	bool RestoreSnapshot(UNiagaraSystem& System, const TArray<FSnapshotEntry>& Snapshot, FString& OutError)
	{
		OutError.Reset();
		FNiagaraUserRedirectionParameterStore& Store = System.GetExposedParameters();
		TArray<FNiagaraVariable> Current;
		Enumerate(System, Current);
		for (const FNiagaraVariable& Variable : Current)
		{
			if (!Store.RemoveParameter(Variable))
			{
				OutError = TEXT("niagara_user_parameter_snapshot_remove_failed:") + Variable.GetName().ToString();
				return false;
			}
		}
		for (const FSnapshotEntry& Entry : Snapshot)
		{
			if (!Store.AddParameter(Entry.Variable, true, true))
			{
				OutError = TEXT("niagara_user_parameter_snapshot_add_failed:") + Entry.Variable.GetName().ToString();
				return false;
			}
			if (!Entry.bHasValue) continue;
			if (Entry.Variable.GetType().IsDataInterface())
			{
				Store.SetDataInterface(Cast<UNiagaraDataInterface>(Entry.ObjectValue.Get()), Entry.Variable);
			}
			else if (Entry.Variable.GetType().IsUObject())
			{
				Store.SetUObject(Entry.ObjectValue.Get(), Entry.Variable);
			}
			else if (Entry.ValueBytes.Num() == Entry.Variable.GetType().GetSize())
			{
				if (!Store.SetParameterData(Entry.ValueBytes.GetData(), Entry.Variable, false))
				{
					OutError = TEXT("niagara_user_parameter_snapshot_set_failed:") + Entry.Variable.GetName().ToString();
					return false;
				}
			}
		}
		return true;
	}

	TSharedRef<FJsonObject> BuildState(
		const UNiagaraSystem& System,
		const FNiagaraVariable& Variable,
		const bool bIncludeValueText)
	{
		return BuildStoreState(System.GetExposedParameters(), Variable, bIncludeValueText);
	}

	TSharedRef<FJsonObject> BuildStoreState(
		const FNiagaraParameterStore& Store,
		const FNiagaraVariable& Variable,
		const bool bIncludeValueText)
	{
		TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
		State->SetStringField(TEXT("type"), UeAgentNiagaraStageModuleAuthoring::GetNiagaraTypeToken(Variable.GetType()));
		State->SetStringField(TEXT("value_kind"), GetValueKind(Variable.GetType()));
		bool bHasValue = false;
		FString ValueText;
		FNiagaraVariable StoreVariable;
		const FNiagaraVariable& ValueVariable = ResolveStoreVariable(Store, Variable, StoreVariable)
			? StoreVariable
			: Variable;
		TSharedPtr<FJsonValue> Value = EncodeValue(Store, ValueVariable, bHasValue, ValueText);
		if (Value.IsValid()) State->SetField(TEXT("value"), Value);
		if (bIncludeValueText) State->SetStringField(TEXT("value_text"), ValueText);
		return State;
	}
}

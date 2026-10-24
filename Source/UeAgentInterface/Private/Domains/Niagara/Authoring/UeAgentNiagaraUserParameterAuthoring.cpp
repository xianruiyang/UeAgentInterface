// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Niagara/Authoring/UeAgentNiagaraUserParameterAuthoring.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Domains/Niagara/Authoring/UeAgentNiagaraStageModuleAuthoring.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/ScopeExit.h"
#include "NiagaraDataInterface.h"
#include "NiagaraParameterStore.h"
#include "NiagaraSystem.h"
#include "UObject/UnrealType.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringEncodedTypes.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringTypeDescriptors.h"

namespace UeAgentNiagaraUserParameterAuthoring
{
	namespace
	{
		UE::UAI::Contract::FDiagnostic ParameterFailure(const TCHAR* Code, const FString& Reason,
			const FString& Field, const TSharedPtr<FJsonValue>& Actual, const TSharedPtr<FJsonValue>& Expected)
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("niagara_user_parameter"));
			Error.PropertyPath = Field;
			Error.ActualValue = Actual;
			Error.ActualValuePresent = Actual.IsValid();
			Error.ExpectedValue = Expected;
			Error.ExpectedValuePresent = Expected.IsValid();
			return Error;
		}

		UE::UAI::Contract::FDiagnostic StoreFailure(const TCHAR* Code, const TCHAR* Action, const FNiagaraVariable& Variable)
		{
			auto Error = ParameterFailure(Code,
				FString::Printf(TEXT("Could not %s user parameter '%s' (%s) in the Niagara parameter store."),
					Action, *Variable.GetName().ToString(), *Variable.GetType().GetName()), TEXT("store_operation_succeeded"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			Error.CandidateName = Variable.GetName().ToString();
			return Error;
		}

		void LocatePatchFailure(UE::UAI::Contract::FDiagnostic& Error, const FPatch& Patch, const FString& Field)
		{
			if (Error.IsEmpty()) return;
			if (Error.CandidateName.IsEmpty()) Error.CandidateName = Patch.ParameterName;
			const TSharedPtr<FJsonObject>* Parameters = nullptr;
			if (!Patch.SourceDocument || !Patch.SourceDocument->TryGetObjectField(TEXT("user_parameters"), Parameters)
				|| !Parameters || !*Parameters) return;
			const auto Entry = (*Parameters)->TryGetField(Patch.SourceKey);
			if (Entry && Entry->Type == EJson::Object)
				UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Entry->AsObject(), Field);
			else UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, **Parameters, Patch.SourceKey);
		}

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

		TSharedRef<FJsonObject> Literal(const float Value)
		{
			const auto Encoded = MakeShared<FJsonValueNumber>(Value);
			UeAgentAuthoringTypeDescriptors::FDescriptor NativeType;
			NativeType.Kind = TEXT("float");
			UeAgentAuthoringEncodedTypes::FScope::Record(Encoded, NativeType, nullptr);
			const auto Result = MakeShared<FJsonObject>();
			Result->SetField(TEXT("literal"), Encoded);
			return Result;
		}

		TSharedRef<FJsonObject> Components(const TArray<double>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> Items;
			Items.Reserve(Values.Num());
			for (const double Value : Values)
			{
				const auto Encoded = MakeShared<FJsonValueNumber>(Value);
				UeAgentAuthoringTypeDescriptors::FDescriptor NativeType;
				NativeType.Kind = TEXT("float"); // Niagara's vector/position/color components are float storage.
				UeAgentAuthoringEncodedTypes::FScope::Record(Encoded, NativeType, nullptr);
				Items.Add(Encoded);
			}
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
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutBytes.Reset();
			OutObject = nullptr;
			if (Type.IsDataInterface() || Type.IsUObject())
			{
				return UeAgentPropertyValueCodec::DecodeObjectReference(Value,
					Type.GetClass(), nullptr, OutObject, OutError);
			}

			OutBytes.SetNumZeroed(Type.GetSize());
			if (Type == FNiagaraTypeDefinition::GetBoolDef())
			{
				bool bValue = false;
				if (!ReadLiteralBool(Value, bValue))
				{
					const bool bObject = Value.IsValid() && Value->Type == EJson::Object;
					OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("niagara_user_parameter_bool_value_invalid"),
						bObject ? TEXT("/literal") : TEXT("/"), MakeShared<FJsonValueString>(TEXT("boolean literal")),
						bObject ? Value->AsObject()->TryGetField(TEXT("literal")) : Value);
					OutError.ActualValuePresent = bObject ? Value->AsObject()->HasField(TEXT("literal")) : Value.IsValid();
					OutError.PublicMessage = TEXT("A Niagara boolean parameter requires a boolean literal.");
					return false;
				}
				FNiagaraBool NiagaraValue;
				NiagaraValue.SetValue(bValue);
				FMemory::Memcpy(OutBytes.GetData(), &NiagaraValue, sizeof(NiagaraValue));
				return true;
			}
			if (Type == FNiagaraTypeDefinition::GetIntDef()
				|| Type == FNiagaraTypeDefinition::GetFloatDef() || Type == FNiagaraTypeHelper::GetDoubleDef())
			{
				// Niagara's scalar structs only wrap their native Value property.
				// Keep the public literal shape and let the common codec handle the
				// original number lexeme, integer width and float conversion.
				auto* Struct = Cast<UScriptStruct>(Type.GetStruct());
				auto* Field = Struct ? FindFProperty<FProperty>(Struct, TEXT("Value")) : nullptr;
				if (!Field) {
					OutError = ParameterFailure(TEXT("niagara_user_parameter_scalar_field_missing"),
						TEXT("The Niagara scalar type has no reflected Value property for decoding."), TEXT("type"),
						MakeShared<FJsonValueString>(Type.GetName()), MakeShared<FJsonValueString>(TEXT("scalar struct with a Value property")));
					OutError.SourceKind = TEXT("native_result");
					return false;
				}
				return UeAgentPropertyValueCodec::Decode(Field, Value,
					Field->ContainerPtrToValuePtr<void>(OutBytes.GetData()), nullptr, {}, OutError);
			}

			int32 ComponentCount = 0;
			if (Type == FNiagaraTypeDefinition::GetVec2Def()) ComponentCount = 2;
			else if (Type == FNiagaraTypeDefinition::GetVec3Def() || Type == FNiagaraTypeDefinition::GetPositionDef()) ComponentCount = 3;
			else if (Type == FNiagaraTypeDefinition::GetVec4Def()
				|| Type == FNiagaraTypeDefinition::GetQuatDef()
				|| Type == FNiagaraTypeDefinition::GetColorDef()) ComponentCount = 4;
			if (ComponentCount > 0)
			{
				// Position is the native FVector3f-derived storage with a distinct
				// simulation-space meaning; only its representation maps to Vec3.
				auto* Struct = Cast<UScriptStruct>((Type == FNiagaraTypeDefinition::GetPositionDef()
					? FNiagaraTypeDefinition::GetVec3Def() : Type).GetStruct());
				return UeAgentPropertyValueCodec::DecodeStructValue(Struct, Value, OutBytes.GetData(), OutError);
			}

			if (Type.IsEnum())
			{
				UEnum* Enum = Type.GetEnum();
				int64 EnumValue = 0;
				if (!UeAgentPropertyValueCodec::DecodeEnumValue(Enum, Value, EnumValue, OutError)) return false;
				if (EnumValue < MIN_int32 || EnumValue > MAX_int32)
				{
					const auto Expected = MakeShared<FJsonObject>();
					Expected->SetStringField(TEXT("enum"), Enum->GetPathName());
					Expected->SetNumberField(TEXT("minimum"), MIN_int32);
					Expected->SetNumberField(TEXT("maximum"), MAX_int32);
					OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("niagara_user_parameter_enum_value_out_of_range"),
						TEXT("/selected"), MakeShared<FJsonValueObject>(Expected), MakeShared<FJsonValueNumberString>(LexToString(EnumValue)));
					OutError.PublicMessage = TEXT("The enum member value does not fit Niagara's signed 32-bit parameter storage.");
					return false;
				}
				const int32 Stored = static_cast<int32>(EnumValue);
				FMemory::Memcpy(OutBytes.GetData(), &Stored, sizeof(Stored));
				return true;
			}

			UScriptStruct* Struct = Cast<UScriptStruct>(Type.GetStruct());
			if (!Struct)
			{
				OutError = ParameterFailure(TEXT("niagara_user_parameter_type_not_structured"),
					TEXT("This Niagara parameter type has no supported reflected structure for decoding."), TEXT("type"),
					MakeShared<FJsonValueString>(Type.GetName()), MakeShared<FJsonValueString>(TEXT("supported scalar, vector, enum, object or reflected struct")));
				OutError.SourceKind = TEXT("native_result");
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
			UE::UAI::Contract::FDiagnostic& OutError)
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
			if (!bSet) OutError = StoreFailure(TEXT("niagara_user_parameter_set_data_failed"), TEXT("set the value of"), Variable);
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

			// The store may contain an SWC simulation layout for an LWC declared
			// type. Let UE convert it back before the ordinary reflected codec reads it.
			TArray<uint8> NativeBytes;
			NativeBytes.SetNumZeroed(Type.GetSize());
			if (!Store.CopyParameterData(Variable, NativeBytes.GetData())) return nullptr;
			const uint8* Data = NativeBytes.GetData();
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
		const FNiagaraUserRedirectionParameterStore& Store = System.GetExposedParameters();
		Store.GetUserParameters(OutVariables);
		// UE enumerates display aliases. Native notifications, ordinary stores and
		// snapshot restoration require the actual namespaced variable identity.
		for (FNiagaraVariable& Variable : OutVariables) Store.RedirectUserVariable(Variable);
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutPatches.Reset();
		OutError.Reset();
		if (!ProviderExtension.IsValid() || !ProviderExtension->HasField(TEXT("user_parameters"))) return true;
		const TSharedPtr<FJsonObject>* Parameters = nullptr;
		if (!ProviderExtension->TryGetObjectField(TEXT("user_parameters"), Parameters)
			|| !Parameters || !Parameters->IsValid())
		{
			OutError = ParameterFailure(TEXT("niagara_workspace_user_parameters_invalid"), TEXT("user_parameters must be an object keyed by parameter name."),
				TEXT("user_parameters"), ProviderExtension->TryGetField(TEXT("user_parameters")), MakeShared<FJsonValueString>(TEXT("object")));
			UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *ProviderExtension, TEXT("user_parameters"));
			return false;
		}
		if ((*Parameters)->Values.IsEmpty()) return true;
		if (!System)
		{
			OutError = ParameterFailure(TEXT("niagara_workspace_user_parameters_require_system"), TEXT("User parameters require a Niagara system target."),
				TEXT("system_target_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *ProviderExtension, TEXT("user_parameters"));
			return false;
		}
		const auto SourceLocations = MakeShared<TMap<FString, UE::UAI::Contract::FSourceLocation>>(
			UeAgentAuthoringDiagnostics::FScopedSourceLocations::CaptureProjection(*ProviderExtension));
		TSet<FString> SeenNames;
		TArray<UeAgentAuthoringDiagnostics::FDiagnostic> Errors;
		UeAgentAuthoringDiagnostics::FDiagnostic FirstError;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Parameters)->Values)
		{
			FString ErrorField;
			OutError.Reset();
			const bool bParsed = [&]()
			{
				FPatch& Patch = OutPatches.AddDefaulted_GetRef();
				Patch.SourceKey = Pair.Key;
				Patch.SourceDocument = ProviderExtension;
				Patch.SourceLocations = SourceLocations;
				Patch.ParameterName = CanonicalizeName(Pair.Key);
				const FString ComparisonName = NormalizeNameForComparison(Patch.ParameterName);
				if (Patch.ParameterName == TEXT("User.") || SeenNames.Contains(ComparisonName))
				{
					OutError = ParameterFailure(TEXT("niagara_workspace_user_parameter_identity_invalid"),
						Patch.ParameterName == TEXT("User.") ? TEXT("A user parameter name must contain a name after the User. prefix.")
							: TEXT("User parameter names must be unique after prefix and case normalization."),
						TEXT("parameter_name"), MakeShared<FJsonValueString>(Pair.Key), MakeShared<FJsonValueString>(TEXT("non-empty unique user parameter name")));
					return false;
				}
				SeenNames.Add(ComparisonName);
				FNiagaraVariable Existing;
				const bool bExists = FindByName(*System, Patch.ParameterName, Existing);
				if (!Pair.Value.IsValid() || Pair.Value->Type == EJson::Null)
				{
					Patch.bRemove = true;
					return true;
				}
				if (Pair.Value->Type != EJson::Object)
				{
					OutError = ParameterFailure(TEXT("niagara_workspace_user_parameter_invalid"), TEXT("A user parameter entry must be an object or null for removal."),
						FString(), Pair.Value, MakeShared<FJsonValueString>(TEXT("object or null")));
					return false;
				}
				const TSharedPtr<FJsonObject> Object = Pair.Value->AsObject();
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Object->Values)
				{
					if (Field.Key != TEXT("type") && Field.Key != TEXT("value")
						&& Field.Key != TEXT("kind") && Field.Key != TEXT("remove")
						&& Field.Key != TEXT("has_value") && Field.Key != TEXT("value_text")
						&& Field.Key != TEXT("authoring_source"))
					{
						ErrorField = Field.Key;
						OutError = ParameterFailure(TEXT("niagara_workspace_user_parameter_unknown_field"),
							TEXT("The user parameter entry contains an unsupported field."), Field.Key, MakeShared<FJsonValueString>(Field.Key),
							MakeShared<FJsonValueString>(TEXT("type, value, kind, remove, has_value, value_text or authoring_source")));
						return false;
					}
				}
				ErrorField = TEXT("has_value");
				bool bDeclaredHasValue = false;
				const bool bHasDeclaredValueState = Object->HasField(TEXT("has_value"));
				if (bHasDeclaredValueState && !Object->TryGetBoolField(TEXT("has_value"), bDeclaredHasValue))
				{
					OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("niagara_workspace_user_parameter_has_value_invalid"),
						TEXT("/has_value"), MakeShared<FJsonValueString>(TEXT("boolean")), Object->TryGetField(TEXT("has_value")), TEXT("preflight"), TEXT("has_value must be a boolean."));
					return false;
				}
				ErrorField = TEXT("value_text");
				FString ValueText;
				if (Object->HasField(TEXT("value_text"))
					&& !Object->TryGetStringField(TEXT("value_text"), ValueText))
				{
					OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("niagara_workspace_user_parameter_value_text_invalid"),
						TEXT("/value_text"), MakeShared<FJsonValueString>(TEXT("string")), Object->TryGetField(TEXT("value_text")), TEXT("preflight"), TEXT("value_text must be a string."));
					return false;
				}
				ErrorField = TEXT("remove");
				Object->TryGetBoolField(TEXT("remove"), Patch.bRemove);
				if (Patch.bRemove) return true;
				ErrorField = TEXT("type");
				FString TypeText;
				if (Object->HasField(TEXT("type")) && !Object->TryGetStringField(TEXT("type"), TypeText))
				{
					OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("niagara_workspace_user_parameter_type_invalid"),
						TEXT("/type"), MakeShared<FJsonValueString>(TEXT("Niagara type name")), Object->TryGetField(TEXT("type")), TEXT("preflight"), TEXT("The user parameter type must be a Niagara type name string."));
					return false;
				}
				Patch.bTypeExplicit = !TypeText.TrimStartAndEnd().IsEmpty();
				if (Patch.bTypeExplicit)
				{
					if (!UeAgentNiagaraStageModuleAuthoring::ResolveNiagaraTypeDefinition(
						TypeText, Patch.TypeDefinition, OutError))
					{
						if (OutError.PropertyPath.IsEmpty()) OutError.PropertyPath = Pair.Key;
						return false;
					}
				}
				else if (bExists)
				{
					Patch.TypeDefinition = Existing.GetType();
				}
				else
				{
					OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("niagara_workspace_user_parameter_type_required"),
						TEXT("/type"), MakeShared<FJsonValueString>(TEXT("Niagara type name for a new parameter")), Object->TryGetField(TEXT("type")), TEXT("preflight"), TEXT("A new user parameter requires an explicit type."));
					OutError.ActualValuePresent = Object->HasField(TEXT("type"));
					return false;
				}
				if (bExists && Existing.GetType() != Patch.TypeDefinition)
				{
					OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("niagara_workspace_user_parameter_type_conflict"), TEXT("/type"), MakeShared<FJsonValueString>(Existing.GetType().GetName()), MakeShared<FJsonValueString>(TypeText), TEXT("input_decode"), TEXT("An existing user parameter cannot change its type."));
					return false;
				}
				ErrorField = TEXT("kind");
				FString ValueKind;
				if (Object->HasField(TEXT("kind")))
				{
					if (!Object->TryGetStringField(TEXT("kind"), ValueKind)
						|| !ValueKind.Equals(GetValueKind(Patch.TypeDefinition), ESearchCase::IgnoreCase))
					{
						OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("niagara_workspace_user_parameter_value_kind_conflict"),
							TEXT("/kind"), MakeShared<FJsonValueString>(GetValueKind(Patch.TypeDefinition)), Object->TryGetField(TEXT("kind")), TEXT("preflight"), TEXT("The user parameter value kind does not match its type."));
						return false;
					}
				}
				ErrorField = TEXT("value");
				Patch.Value = Object->TryGetField(TEXT("value"));
				Patch.bHasValue = Patch.Value.IsValid();
				if (bHasDeclaredValueState && !bDeclaredHasValue && Patch.bHasValue)
				{
					ErrorField = TEXT("has_value");
					OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("niagara_workspace_user_parameter_has_value_conflict"), TEXT("/has_value"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), TEXT("input_decode"), TEXT("has_value is false but a value is present."));
					return false;
				}
				if (Patch.bHasValue)
				{
					TArray<uint8> ValidationBytes;
					UObject* ValidationObject = nullptr;
					if (!DecodeValue(Patch.TypeDefinition, Patch.Value, ValidationBytes, ValidationObject, OutError))
					{
						// Preserve the codec's structured cause, including nested field diagnostics.
						UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, TEXT("/value"));
						if (OutError.Stage.IsEmpty()) OutError.Stage = TEXT("preflight");
						return false;
					}
					if (UScriptStruct* Struct = Cast<UScriptStruct>(Patch.TypeDefinition.GetStruct()))
					{
						if (GetValueKind(Patch.TypeDefinition) == TEXT("struct")) Struct->DestroyStruct(ValidationBytes.GetData());
					}
				}
				return true;
			}();
			if (bParsed) continue;
			auto Diagnostic = OutError;
			if (Diagnostic.Stage.IsEmpty()) Diagnostic.Stage = TEXT("preflight");
			Diagnostic.TargetPath = System->GetPathName();
			if (Diagnostic.CandidateName.IsEmpty()) Diagnostic.CandidateName = Pair.Key;
			if (Diagnostic.PropertyPath.IsEmpty()) Diagnostic.PropertyPath = ErrorField;
			if (Pair.Value.IsValid() && Pair.Value->Type == EJson::Object)
				UeAgentAuthoringDiagnostics::LocateAuthoringSource(Diagnostic, *Pair.Value->AsObject(), ErrorField);
			else UeAgentAuthoringDiagnostics::LocateAuthoringSource(Diagnostic, **Parameters, Pair.Key);
			if (Errors.IsEmpty()) FirstError = Diagnostic;
			Errors.Add(MoveTemp(Diagnostic));
		}
		if (!Errors.IsEmpty())
		{
			OutPatches.Reset();
			FirstError.Diagnostics = MoveTemp(Errors);
			OutError = MoveTemp(FirstError);
			return false;
		}
		OutError.Reset();
		return true;
	}

	bool ApplyPatches(UNiagaraSystem& System, const TArray<FPatch>& Patches, UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutError.Reset();
		if (Patches.IsEmpty()) return true;
		System.Modify();
		FNiagaraUserRedirectionParameterStore& Store = System.GetExposedParameters();
		for (const FPatch& Patch : Patches)
		{
			UeAgentAuthoringDiagnostics::FScopedSourceLocations Sources(Patch.SourceDocument.Get(), [&Patch](const FString& Pointer) {
				return Patch.SourceLocations ? UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(*Patch.SourceLocations, Pointer)
					: UE::UAI::Contract::FSourceLocation();
			});
			FString ErrorField;
			ON_SCOPE_EXIT { LocatePatchFailure(OutError, Patch, ErrorField); };
			FNiagaraVariable Existing;
			const bool bExists = FindByName(System, Patch.ParameterName, Existing);
			if (Patch.bRemove)
			{
				if (bExists && !Store.RemoveParameter(Existing))
				{
					OutError = StoreFailure(TEXT("niagara_workspace_user_parameter_remove_failed"), TEXT("remove"), Existing);
					return false;
				}
				if (bExists) System.HandleVariableRemoved(Existing, true);
				continue;
			}
			FNiagaraVariable Variable = Existing;
			if (!bExists)
			{
				Variable = FNiagaraVariable(Patch.TypeDefinition, FName(*Patch.ParameterName));
				FNiagaraUserRedirectionParameterStore::MakeUserVariable(Variable);
				if (!Store.AddParameter(Variable, true, true))
				{
					OutError = StoreFailure(TEXT("niagara_workspace_user_parameter_add_failed"), TEXT("add"), Variable);
					return false;
				}
				if (!FindByName(System, Patch.ParameterName, Variable))
				{
					OutError = ParameterFailure(TEXT("niagara_workspace_user_parameter_missing_after_add"),
						TEXT("The user parameter is still absent after the store reported adding it."), TEXT("parameter_present"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return false;
				}
			}
			if (Patch.bHasValue && !SetValue(Store, Variable, Patch.Value, OutError))
			{
				ErrorField = TEXT("value");
				UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, TEXT("/value"));
				return false;
			}
		}
		return true;
	}

	bool ApplyValuePatchesToStore(
		const UNiagaraSystem& System,
		FNiagaraParameterStore& Store,
		const TArray<FPatch>& Patches,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutError.Reset();
		for (const FPatch& Patch : Patches)
		{
			UeAgentAuthoringDiagnostics::FScopedSourceLocations Sources(Patch.SourceDocument.Get(), [&Patch](const FString& Pointer) {
				return Patch.SourceLocations ? UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(*Patch.SourceLocations, Pointer)
					: UE::UAI::Contract::FSourceLocation();
			});
			FString ErrorField;
			ON_SCOPE_EXIT { LocatePatchFailure(OutError, Patch, ErrorField); };
			FNiagaraVariable Variable;
			if (!FindByName(System, Patch.ParameterName, Variable))
			{
				OutError = ParameterFailure(TEXT("niagara_runtime_user_parameter_not_found"), TEXT("The requested user parameter does not exist in this Niagara system."),
					TEXT("parameter_name"), MakeShared<FJsonValueString>(Patch.ParameterName), MakeShared<FJsonValueString>(TEXT("existing user parameter name")));
				return false;
			}
			if (Patch.bRemove)
			{
				OutError = ParameterFailure(TEXT("niagara_runtime_user_parameter_remove_unsupported"), TEXT("Runtime parameter patches can update values but cannot remove parameters."),
					TEXT("remove"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
				ErrorField = TEXT("remove");
				return false;
			}
			if (Variable.GetType() != Patch.TypeDefinition)
			{
				OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("niagara_runtime_user_parameter_type_conflict"), TEXT("/type"), MakeShared<FJsonValueString>(Variable.GetType().GetName()), MakeShared<FJsonValueString>(Patch.TypeDefinition.GetName()), TEXT("runtime_apply"), TEXT("The runtime user parameter patch must retain the system parameter type."));
				ErrorField = TEXT("type");
				OutError.CandidateName = Patch.ParameterName;
				OutError.bRetrySafe = false;
				return false;
			}
			FNiagaraVariable StoreVariable;
			if (!ResolveStoreVariable(Store, Variable, StoreVariable))
			{
				OutError = ParameterFailure(TEXT("niagara_runtime_user_parameter_not_in_store"),
					FString::Printf(TEXT("The runtime store contains no '%s' user parameter of type '%s'."), *Patch.ParameterName, *Variable.GetType().GetName()),
					TEXT("parameter_in_runtime_store"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				return false;
			}
			if (Patch.bHasValue && !SetValue(Store, StoreVariable, Patch.Value, OutError))
			{
				ErrorField = TEXT("value");
				UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, TEXT("/value"));
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
			else
			{
				Entry.ValueBytes.SetNumZeroed(Variable.GetType().GetSize());
				Entry.bHasValue = Store.CopyParameterData(Variable, Entry.ValueBytes.GetData());
				if (!Entry.bHasValue) Entry.ValueBytes.Reset();
			}
		}
	}

	bool RestoreSnapshot(UNiagaraSystem& System, const TArray<FSnapshotEntry>& Snapshot, UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutError.Reset();
		ON_SCOPE_EXIT { if (!OutError.IsEmpty()) OutError.SourceKind = TEXT("native_result"); };
		FNiagaraUserRedirectionParameterStore& Store = System.GetExposedParameters();
		TArray<FNiagaraVariable> Current;
		Enumerate(System, Current);
		for (const FNiagaraVariable& Variable : Current)
		{
			if (!Store.RemoveParameter(Variable))
			{
				OutError = StoreFailure(TEXT("niagara_user_parameter_snapshot_remove_failed"), TEXT("remove while restoring the snapshot of"), Variable);
				return false;
			}
		}
		for (const FSnapshotEntry& Entry : Snapshot)
		{
			if (!Store.AddParameter(Entry.Variable, true, true))
			{
				OutError = StoreFailure(TEXT("niagara_user_parameter_snapshot_add_failed"), TEXT("recreate from the snapshot of"), Entry.Variable);
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
					OutError = StoreFailure(TEXT("niagara_user_parameter_snapshot_set_failed"), TEXT("restore the saved value of"), Entry.Variable);
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
		State->SetStringField(TEXT("kind"), GetValueKind(Variable.GetType()));
		bool bHasValue = false;
		FString ValueText;
		FNiagaraVariable StoreVariable;
		const FNiagaraVariable& ValueVariable = ResolveStoreVariable(Store, Variable, StoreVariable)
			? StoreVariable
			: Variable;
		TSharedPtr<FJsonValue> Value = EncodeValue(Store, ValueVariable, bHasValue, ValueText);
		if (Value.IsValid())
		{
			UeAgentAuthoringTypeDescriptors::FDescriptor NativeType;
			NativeType.Kind = GetValueKind(ValueVariable.GetType());
			if (const UEnum* Enum = ValueVariable.GetType().GetEnum()) NativeType.EnumPath = Enum->GetPathName();
			else if (const UScriptStruct* Struct = ValueVariable.GetType().GetScriptStruct())
			{
				const auto StructType = UeAgentAuthoringTypeDescriptors::DescribeStruct(Struct);
				if (StructType.Kind == NativeType.Kind || NativeType.Kind == TEXT("position")) NativeType = StructType;
			}
			UeAgentAuthoringEncodedTypes::FScope::Record(Value, NativeType, nullptr);
			State->SetField(TEXT("value"), Value);
		}
		if (bIncludeValueText) State->SetStringField(TEXT("value_text"), ValueText);
		return State;
	}
}

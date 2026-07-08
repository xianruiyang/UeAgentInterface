// Copyright Epic Games, Inc. All Rights Reserved.

#include "Commands/Audio/UeAgentHttpServer_AudioOps.h"
#include "Utilities/UeAgentJsonDiagnostics.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Sound/SoundAttenuation.h"
#include "UObject/UnrealType.h"

namespace UeAgentAudioOps
{
	FProperty* FindPropertyByNameLoose(UStruct* Struct, const FString& Name)
	{
		if (!Struct || Name.IsEmpty())
		{
			return nullptr;
		}
		if (FProperty* Direct = Struct->FindPropertyByName(*Name))
		{
			return Direct;
		}

		FString Normalized = Name;
		Normalized.ReplaceInline(TEXT("_"), TEXT(""));
		Normalized.ReplaceInline(TEXT("-"), TEXT(""));
		FString NormalizedWithoutB = Normalized;
		if (NormalizedWithoutB.StartsWith(TEXT("b"), ESearchCase::CaseSensitive))
		{
			NormalizedWithoutB = NormalizedWithoutB.Mid(1);
		}

		for (TFieldIterator<FProperty> It(Struct, EFieldIterationFlags::IncludeSuper); It; ++It)
		{
			FProperty* Property = *It;
			if (!Property)
			{
				continue;
			}
			FString PropertyName = Property->GetName();
			FString PropertyNameNormalized = PropertyName;
			PropertyNameNormalized.ReplaceInline(TEXT("_"), TEXT(""));
			PropertyNameNormalized.ReplaceInline(TEXT("-"), TEXT(""));
			FString PropertyNameWithoutB = PropertyNameNormalized.StartsWith(TEXT("b")) ? PropertyNameNormalized.Mid(1) : PropertyNameNormalized;
			if (PropertyName.Equals(Name, ESearchCase::IgnoreCase)
				|| PropertyNameNormalized.Equals(Normalized, ESearchCase::IgnoreCase)
				|| PropertyNameWithoutB.Equals(NormalizedWithoutB, ESearchCase::IgnoreCase)
				|| Property->GetAuthoredName().Equals(Name, ESearchCase::IgnoreCase))
			{
				return Property;
			}
		}
		return nullptr;
	}
	bool ResolvePropertyPath(UObject* Obj, const FString& PropertyPath, FProperty*& OutProperty, void*& OutValuePtr)
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
	bool ExportPropertyText(UObject* Obj, const FString& PropertyPath, FString& OutText, FString& OutCppType)
	{
		FProperty* Property = nullptr;
		void* ValuePtr = nullptr;
		if (!ResolvePropertyPath(Obj, PropertyPath, Property, ValuePtr) || !Property || !ValuePtr)
		{
			return false;
		}
		OutText.Reset();
		Property->ExportTextItem_Direct(OutText, ValuePtr, nullptr, Obj, PPF_None);
		OutCppType = Property->GetCPPType();
		return true;
	}
	bool ImportPropertyText(UObject* Obj, const FString& PropertyPath, const FString& ValueText, FString& OutReadback, FString& OutError)
	{
		FProperty* Property = nullptr;
		void* ValuePtr = nullptr;
		if (!ResolvePropertyPath(Obj, PropertyPath, Property, ValuePtr) || !Property || !ValuePtr)
		{
			OutError = TEXT("property_not_found");
			return false;
		}
		Obj->Modify();
		const TCHAR* Result = Property->ImportText_Direct(*ValueText, ValuePtr, Obj, PPF_None);
		if (!Result)
		{
			OutError = TEXT("property_import_failed");
			return false;
		}
		Property->ExportTextItem_Direct(OutReadback, ValuePtr, nullptr, Obj, PPF_None);
		Obj->PostEditChange();
		Obj->MarkPackageDirty();
		return true;
	}
	FString JsonValueToImportText(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
		{
			return FString();
		}
		switch (Value->Type)
		{
		case EJson::String:
			return Value->AsString();
		case EJson::Number:
			return FString::SanitizeFloat(Value->AsNumber());
		case EJson::Boolean:
			return Value->AsBool() ? TEXT("True") : TEXT("False");
		case EJson::Object:
		case EJson::Array:
		{
			FString JsonText;
			UeAgentJsonDiagnostics::SerializeJsonValueCompact(Value, JsonText);
			return JsonText;
		}
		default:
			return FString();
		}
	}
	bool ApplyProperty(UObject* Asset, const FString& PropertyPath, const FString& ValueText, TArray<TSharedPtr<FJsonValue>>& OutResults, bool bDryRun, FString& OutError)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("property_path"), PropertyPath);
		Result->SetStringField(TEXT("requested_value_text"), ValueText);
		if (bDryRun)
		{
			FString Existing;
			FString CppType;
			Result->SetBoolField(TEXT("applied"), false);
			Result->SetBoolField(TEXT("dry_run"), true);
			if (ExportPropertyText(Asset, PropertyPath, Existing, CppType))
			{
				Result->SetStringField(TEXT("property_value_read_back"), Existing);
				Result->SetStringField(TEXT("property_import_status"), TEXT("would_apply"));
			}
			else
			{
				Result->SetStringField(TEXT("property_import_status"), TEXT("property_not_found"));
			}
			OutResults.Add(MakeShared<FJsonValueObject>(Result));
			return true;
		}

		FString Readback;
		FString Error;
		const bool bOk = ImportPropertyText(Asset, PropertyPath, ValueText, Readback, Error);
		Result->SetBoolField(TEXT("applied"), bOk);
		Result->SetStringField(TEXT("property_value_read_back"), Readback);
		Result->SetStringField(TEXT("property_import_status"), bOk ? TEXT("ok") : Error);
		OutResults.Add(MakeShared<FJsonValueObject>(Result));
		if (!bOk)
		{
			OutError = Error;
			return false;
		}
		return true;
	}
	bool ApplyPropertiesArray(UObject* Asset, const TSharedPtr<FJsonObject>& Root, TArray<TSharedPtr<FJsonValue>>& OutResults, bool bDryRun, FString& OutError)
	{
		const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
		if (!Root.IsValid() || !Root->TryGetArrayField(TEXT("properties"), Properties) || !Properties)
		{
			return true;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Properties)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(Obj) || !Obj || !Obj->IsValid())
			{
				continue;
			}
			FString Name;
			FString ValueText;
			if (!(*Obj)->TryGetStringField(TEXT("property_path"), Name))
			{
				(*Obj)->TryGetStringField(TEXT("name"), Name);
			}
			(*Obj)->TryGetStringField(TEXT("value_text"), ValueText);
			if (!Name.IsEmpty() && !ApplyProperty(Asset, Name, ValueText, OutResults, bDryRun, OutError))
			{
				return false;
			}
		}
		return true;
	}
	bool ApplyPropertiesObjectWithPrefix(UObject* Asset, const TSharedPtr<FJsonObject>& Root, const TCHAR* ObjectField, const FString& Prefix, TArray<TSharedPtr<FJsonValue>>& OutResults, bool bDryRun, FString& OutError)
	{
		const TSharedPtr<FJsonObject>* Props = nullptr;
		if (!Root.IsValid() || !Root->TryGetObjectField(ObjectField, Props) || !Props || !Props->IsValid())
		{
			return true;
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Props)->Values)
		{
			const FString PropertyPath = Prefix.IsEmpty() ? Pair.Key : Prefix + TEXT(".") + Pair.Key;
			if (!ApplyProperty(Asset, PropertyPath, JsonValueToImportText(Pair.Value), OutResults, bDryRun, OutError))
			{
				return false;
			}
		}
		return true;
	}
	bool ApplyJsonFieldToProperty(UObject* Asset, const TSharedPtr<FJsonObject>& Obj, const TCHAR* FieldName, const FString& PropertyPath, TArray<TSharedPtr<FJsonValue>>& OutResults, bool bDryRun, FString& OutError)
	{
		if (!Obj.IsValid())
		{
			return true;
		}
		const TSharedPtr<FJsonValue>* Value = Obj->Values.Find(FieldName);
		if (!Value || !Value->IsValid())
		{
			return true;
		}
		return ApplyProperty(Asset, PropertyPath, JsonValueToImportText(*Value), OutResults, bDryRun, OutError);
	}
	bool ApplySoundAttenuationSemanticJson(USoundAttenuation* Asset, const TSharedPtr<FJsonObject>& Root, TArray<TSharedPtr<FJsonValue>>& OutResults, bool bDryRun, FString& OutError)
	{
		if (!Asset || !Root.IsValid())
		{
			return true;
		}

		const TSharedPtr<FJsonObject>* AttenuationObj = nullptr;
		if (Root->TryGetObjectField(TEXT("attenuation"), AttenuationObj) && AttenuationObj && AttenuationObj->IsValid())
		{
			const TSharedPtr<FJsonObject>& A = *AttenuationObj;
			if (!ApplyJsonFieldToProperty(Asset, A, TEXT("attenuate"), TEXT("Attenuation.bAttenuate"), OutResults, bDryRun, OutError)
				|| !ApplyJsonFieldToProperty(Asset, A, TEXT("shape"), TEXT("Attenuation.AttenuationShape"), OutResults, bDryRun, OutError)
				|| !ApplyJsonFieldToProperty(Asset, A, TEXT("falloff_distance"), TEXT("Attenuation.FalloffDistance"), OutResults, bDryRun, OutError)
				|| !ApplyJsonFieldToProperty(Asset, A, TEXT("distance_algorithm"), TEXT("Attenuation.DistanceAlgorithm"), OutResults, bDryRun, OutError)
				|| !ApplyJsonFieldToProperty(Asset, A, TEXT("spatialize"), TEXT("Attenuation.bSpatialize"), OutResults, bDryRun, OutError)
				|| !ApplyJsonFieldToProperty(Asset, A, TEXT("occlusion"), TEXT("Attenuation.bEnableOcclusion"), OutResults, bDryRun, OutError)
				|| !ApplyJsonFieldToProperty(Asset, A, TEXT("occlusion_low_pass_filter_frequency"), TEXT("Attenuation.OcclusionLowPassFilterFrequency"), OutResults, bDryRun, OutError)
				|| !ApplyJsonFieldToProperty(Asset, A, TEXT("occlusion_volume_attenuation"), TEXT("Attenuation.OcclusionVolumeAttenuation"), OutResults, bDryRun, OutError))
			{
				return false;
			}
			double InnerRadius = 0.0;
			if (A->TryGetNumberField(TEXT("inner_radius"), InnerRadius))
			{
				const FString Extents = FString::Printf(TEXT("(X=%s,Y=0.000000,Z=0.000000)"), *FString::SanitizeFloat(InnerRadius));
				if (!ApplyProperty(Asset, TEXT("Attenuation.AttenuationShapeExtents"), Extents, OutResults, bDryRun, OutError))
				{
					return false;
				}
			}
		}

		const TSharedPtr<FJsonObject>* SpatializationObj = nullptr;
		if (Root->TryGetObjectField(TEXT("spatialization"), SpatializationObj) && SpatializationObj && SpatializationObj->IsValid())
		{
			const TSharedPtr<FJsonObject>& S = *SpatializationObj;
			if (!ApplyJsonFieldToProperty(Asset, S, TEXT("spatialize"), TEXT("Attenuation.bSpatialize"), OutResults, bDryRun, OutError)
				|| !ApplyJsonFieldToProperty(Asset, S, TEXT("algorithm"), TEXT("Attenuation.SpatializationAlgorithm"), OutResults, bDryRun, OutError)
				|| !ApplyJsonFieldToProperty(Asset, S, TEXT("stereo_spread"), TEXT("Attenuation.StereoSpread"), OutResults, bDryRun, OutError))
			{
				return false;
			}
		}

		const TSharedPtr<FJsonObject>* OcclusionObj = nullptr;
		if (Root->TryGetObjectField(TEXT("occlusion"), OcclusionObj) && OcclusionObj && OcclusionObj->IsValid())
		{
			const TSharedPtr<FJsonObject>& O = *OcclusionObj;
			if (!ApplyJsonFieldToProperty(Asset, O, TEXT("enabled"), TEXT("Attenuation.bEnableOcclusion"), OutResults, bDryRun, OutError)
				|| !ApplyJsonFieldToProperty(Asset, O, TEXT("trace_channel"), TEXT("Attenuation.OcclusionTraceChannel"), OutResults, bDryRun, OutError)
				|| !ApplyJsonFieldToProperty(Asset, O, TEXT("low_pass_filter_frequency"), TEXT("Attenuation.OcclusionLowPassFilterFrequency"), OutResults, bDryRun, OutError)
				|| !ApplyJsonFieldToProperty(Asset, O, TEXT("volume_attenuation"), TEXT("Attenuation.OcclusionVolumeAttenuation"), OutResults, bDryRun, OutError)
				|| !ApplyJsonFieldToProperty(Asset, O, TEXT("interpolation_time"), TEXT("Attenuation.OcclusionInterpolationTime"), OutResults, bDryRun, OutError))
			{
				return false;
			}
		}

		return true;
	}
}

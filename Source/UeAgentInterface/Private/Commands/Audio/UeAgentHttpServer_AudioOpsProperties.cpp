// Copyright Epic Games, Inc. All Rights Reserved.

#include "Commands/Audio/UeAgentHttpServer_AudioOps.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Sound/SoundAttenuation.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringDirectApply.h"

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
		if (!ResolvePropertyPath(Obj, PropertyPath, Property, ValuePtr) || !Property || !ValuePtr) return false;
		OutText.Reset();
		Property->ExportTextItem_Direct(OutText, ValuePtr, nullptr, Obj, PPF_None);
		OutCppType = Property->GetCPPType();
		return true;
	}
	bool ApplyProperty(UObject* Asset, const FString& PropertyPath, const TSharedPtr<FJsonValue>& Value, TArray<TSharedPtr<FJsonValue>>& OutResults, bool bDryRun, FString& OutError)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("property_path"), PropertyPath);
		Result->SetField(TEXT("requested_value"), Value);
		if (!Asset || PropertyPath.IsEmpty() || !Value.IsValid())
		{
			Result->SetStringField(TEXT("status"), TEXT("invalid_request"));
			OutResults.Add(MakeShared<FJsonValueObject>(Result));
			OutError = TEXT("audio_property_apply_request_invalid");
			return false;
		}

		TStrongObjectPtr<UObject> DryRunTarget;
		UObject* Target = Asset;
		if (bDryRun)
		{
			DryRunTarget.Reset(StaticDuplicateObject(
				Asset, GetTransientPackage(), NAME_None, RF_Transient));
			Target = DryRunTarget.Get();
		}
		if (!Target)
		{
			Result->SetStringField(TEXT("status"), TEXT("dry_run_clone_failed"));
			OutResults.Add(MakeShared<FJsonValueObject>(Result));
			OutError = TEXT("audio_property_dry_run_clone_failed");
			return false;
		}

		TSharedRef<FJsonObject> Descriptor = MakeShared<FJsonObject>();
		Descriptor->SetStringField(TEXT("property_path"), PropertyPath);
		Descriptor->SetField(TEXT("value"), Value);
		UeAgentAuthoringObjectApply::FOptions Options;
		Options.bAllowDomainAuthorizedOwners = true;
		UeAgentAuthoringDirectApply::FResult ApplyResult;
		if (!UeAgentAuthoringDirectApply::ApplyObjectPatch(
				Target, {MakeShared<FJsonValueObject>(Descriptor)}, Options,
				ApplyResult))
		{
			Result->SetStringField(TEXT("status"), ApplyResult.Status);
			Result->SetStringField(TEXT("phase"), ApplyResult.Phase);
			Result->SetStringField(TEXT("error_code"), ApplyResult.ErrorCode);
			Result->SetStringField(TEXT("error_detail"), ApplyResult.ErrorDetail);
			OutResults.Add(MakeShared<FJsonValueObject>(Result));
			OutError = ApplyResult.ErrorCode;
			if (!ApplyResult.ErrorDetail.IsEmpty())
				OutError += TEXT(":") + ApplyResult.ErrorDetail;
			return false;
		}
		Result->SetBoolField(TEXT("applied"), !bDryRun);
		Result->SetBoolField(TEXT("dry_run"), bDryRun);
		Result->SetStringField(TEXT("status"), bDryRun ? TEXT("validated") : TEXT("applied"));
		OutResults.Add(MakeShared<FJsonValueObject>(Result));
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
			(*Obj)->TryGetStringField(TEXT("property_path"), Name);
			const TSharedPtr<FJsonValue> CanonicalValue = (*Obj)->TryGetField(TEXT("value"));
			if (!Name.IsEmpty() && CanonicalValue.IsValid() && !ApplyProperty(Asset, Name, CanonicalValue, OutResults, bDryRun, OutError))
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
			if (!ApplyProperty(Asset, PropertyPath, Pair.Value, OutResults, bDryRun, OutError))
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
		return ApplyProperty(Asset, PropertyPath, *Value, OutResults, bDryRun, OutError);
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
				TSharedPtr<FJsonObject> Extents = MakeShared<FJsonObject>();
				Extents->SetArrayField(TEXT("components"), { MakeShared<FJsonValueNumber>(InnerRadius), MakeShared<FJsonValueNumber>(0.0), MakeShared<FJsonValueNumber>(0.0) });
				if (!ApplyProperty(Asset, TEXT("Attenuation.AttenuationShapeExtents"), MakeShared<FJsonValueObject>(Extents), OutResults, bDryRun, OutError))
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

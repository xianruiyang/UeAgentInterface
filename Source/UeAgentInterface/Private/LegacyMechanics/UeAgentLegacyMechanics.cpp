// Copyright Epic Games, Inc. All Rights Reserved.

#include "LegacyMechanics/UeAgentLegacyMechanics.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/UnrealType.h"
#include "Utilities/UeAgentJsonDiagnostics.h"

bool FUeAgentLegacyMechanics::JsonTryGetObject(
	const TSharedPtr<FJsonObject>& Obj,
	const FString& Key,
	TSharedPtr<FJsonObject>& OutObj)
{
	if (!Obj.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* Value = nullptr;
	if (Obj->TryGetObjectField(Key, Value) && Value && Value->IsValid())
	{
		OutObj = *Value;
		return true;
	}
	return false;
}

bool FUeAgentLegacyMechanics::JsonTryGetString(
	const TSharedPtr<FJsonObject>& Obj,
	const FString& Key,
	FString& OutValue)
{
	return Obj.IsValid() && Obj->TryGetStringField(Key, OutValue);
}

bool FUeAgentLegacyMechanics::JsonTryGetNumber(
	const TSharedPtr<FJsonObject>& Obj,
	const FString& Key,
	double& OutValue)
{
	return UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(
		Obj,
		{Key},
		OutValue);
}

bool FUeAgentLegacyMechanics::JsonTryGetBool(
	const TSharedPtr<FJsonObject>& Obj,
	const FString& Key,
	bool& OutValue)
{
	return Obj.IsValid() && Obj->TryGetBoolField(Key, OutValue);
}

bool FUeAgentLegacyMechanics::JsonTryGetVector(
	const TSharedPtr<FJsonObject>& Obj,
	const FString& Key,
	FVector& OutValue)
{
	const TSharedPtr<FJsonValue>* FieldValue =
		UeAgentJsonDiagnostics::FindFieldValue(Obj, Key);
	return FieldValue
		&& UeAgentJsonDiagnostics::TryReadVectorValue(
			*FieldValue,
			OutValue);
}

bool FUeAgentLegacyMechanics::JsonTryGetRotator(
	const TSharedPtr<FJsonObject>& Obj,
	const FString& Key,
	FRotator& OutValue)
{
	const TSharedPtr<FJsonValue>* FieldValue =
		UeAgentJsonDiagnostics::FindFieldValue(Obj, Key);
	return FieldValue
		&& UeAgentJsonDiagnostics::TryReadRotatorValue(
			*FieldValue,
			OutValue);
}

TSharedPtr<FJsonObject> FUeAgentLegacyMechanics::VecToJson(
	const FVector& Value)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("x"), Value.X);
	Result->SetNumberField(TEXT("y"), Value.Y);
	Result->SetNumberField(TEXT("z"), Value.Z);
	return Result;
}

TSharedPtr<FJsonObject> FUeAgentLegacyMechanics::RotToJson(
	const FRotator& Value)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("pitch"), Value.Pitch);
	Result->SetNumberField(TEXT("yaw"), Value.Yaw);
	Result->SetNumberField(TEXT("roll"), Value.Roll);
	return Result;
}

void FUeAgentLegacyMechanics::SetPropertyImportResultFields(
	const TSharedPtr<FJsonObject>& Obj,
	FProperty* Property,
	const FString& RequestedValueText,
	const FString& AppliedValueText,
	const FString& ImportStatus,
	const FString& ImportError)
{
	if (!Obj.IsValid())
	{
		return;
	}

	const bool bReadBackValue =
		ImportStatus.Equals(
			TEXT("imported_and_read_back"),
			ESearchCase::CaseSensitive)
		|| ImportStatus.Equals(
			TEXT("assigned_object_reference"),
			ESearchCase::CaseSensitive);
	const bool bExactMatch = bReadBackValue
		&& AppliedValueText.Equals(
			RequestedValueText,
			ESearchCase::CaseSensitive);
	Obj->SetStringField(TEXT("requested_value_text"), RequestedValueText);
	Obj->SetStringField(TEXT("applied_value_text"), AppliedValueText);
	Obj->SetBoolField(TEXT("property_value_read_back"), bReadBackValue);
	Obj->SetBoolField(TEXT("value_text_exact_match"), bExactMatch);
	Obj->SetBoolField(
		TEXT("value_text_changed_after_import"),
		bReadBackValue && !bExactMatch);
	Obj->SetStringField(TEXT("property_import_status"), ImportStatus);
	Obj->SetBoolField(TEXT("property_import_verified"), bReadBackValue);
	Obj->SetStringField(
		TEXT("property_import_error"),
		bReadBackValue
			? TEXT("")
			: (ImportError.IsEmpty() ? ImportStatus : ImportError));
	Obj->SetStringField(
		TEXT("cpp_type"),
		Property ? Property->GetCPPType() : TEXT(""));
}

TSharedPtr<FJsonObject>
FUeAgentLegacyMechanics::MakePropertyImportResultJson(
	const FString& PropertyName,
	FProperty* Property,
	const FString& RequestedValueText,
	const FString& AppliedValueText,
	const FString& ImportStatus,
	const FString& ImportError)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("property_name"), PropertyName);
	SetPropertyImportResultFields(
		Result,
		Property,
		RequestedValueText,
		AppliedValueText,
		ImportStatus,
		ImportError);
	return Result;
}

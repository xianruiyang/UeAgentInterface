// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace UeAgentJsonDiagnostics
{
	static FString ResolveProjectRelativeFilePath(const FString& FilePathInput)
	{
		FString FilePath = FilePathInput;
		FilePath.TrimStartAndEndInline();
		if (FilePath.IsEmpty())
		{
			return FilePath;
		}
		if (FPaths::IsRelative(FilePath))
		{
			return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), FilePath));
		}
		return FPaths::ConvertRelativePathToFull(FilePath);
	}

	static FString JsonValueTypeToString(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
		{
			return TEXT("missing");
		}

		switch (Value->Type)
		{
		case EJson::None: return TEXT("none");
		case EJson::Null: return TEXT("null");
		case EJson::String: return TEXT("string");
		case EJson::Number: return TEXT("number");
		case EJson::Boolean: return TEXT("bool");
		case EJson::Array: return TEXT("array");
		case EJson::Object: return TEXT("object");
		default: return TEXT("unknown");
		}
	}

	static TSharedPtr<FJsonObject> MakeIssue(
		const FString& Severity,
		const FString& Code,
		const FString& Path,
		const FString& Message)
	{
		TSharedPtr<FJsonObject> IssueObj = MakeShared<FJsonObject>();
		IssueObj->SetStringField(TEXT("severity"), Severity);
		IssueObj->SetStringField(TEXT("code"), Code);
		IssueObj->SetStringField(TEXT("path"), Path);
		IssueObj->SetStringField(TEXT("message"), Message);
		return IssueObj;
	}

	static void AddIssue(
		TArray<TSharedPtr<FJsonValue>>& Issues,
		const FString& Severity,
		const FString& Code,
		const FString& Path,
		const FString& Message)
	{
		Issues.Add(MakeShared<FJsonValueObject>(MakeIssue(Severity, Code, Path, Message)));
	}

	static int32 EditDistance(const FString& AInput, const FString& BInput)
	{
		const FString A = AInput.ToLower();
		const FString B = BInput.ToLower();
		const int32 ANum = A.Len();
		const int32 BNum = B.Len();

		TArray<int32> Previous;
		TArray<int32> Current;
		Previous.SetNumUninitialized(BNum + 1);
		Current.SetNumUninitialized(BNum + 1);
		for (int32 BIndex = 0; BIndex <= BNum; ++BIndex)
		{
			Previous[BIndex] = BIndex;
		}

		for (int32 AIndex = 1; AIndex <= ANum; ++AIndex)
		{
			Current[0] = AIndex;
			for (int32 BIndex = 1; BIndex <= BNum; ++BIndex)
			{
				const int32 ReplaceCost = A[AIndex - 1] == B[BIndex - 1] ? 0 : 1;
				Current[BIndex] = FMath::Min3(
					Previous[BIndex] + 1,
					Current[BIndex - 1] + 1,
					Previous[BIndex - 1] + ReplaceCost);
			}
			Swap(Previous, Current);
		}

		return Previous[BNum];
	}

	static FString FindClosestKnownField(const FString& FieldName, const TArray<FString>& KnownFields)
	{
		int32 BestDistance = MAX_int32;
		FString BestField;
		for (const FString& KnownField : KnownFields)
		{
			const int32 Distance = EditDistance(FieldName, KnownField);
			if (Distance < BestDistance)
			{
				BestDistance = Distance;
				BestField = KnownField;
			}
		}

		const int32 MaxUsefulDistance = FMath::Max(2, FMath::Min(FieldName.Len(), BestField.Len()) / 3);
		return BestDistance <= MaxUsefulDistance ? BestField : FString();
	}

	static void WarnUnknownFields(
		const TSharedPtr<FJsonObject>& Obj,
		const FString& Path,
		const TArray<FString>& KnownFields,
		TArray<TSharedPtr<FJsonValue>>& Warnings)
	{
		if (!Obj.IsValid())
		{
			return;
		}

		TSet<FString> KnownLower;
		for (const FString& KnownField : KnownFields)
		{
			KnownLower.Add(KnownField.ToLower());
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Obj->Values)
		{
			if (KnownLower.Contains(Pair.Key.ToLower()))
			{
				continue;
			}

			const FString Suggestion = FindClosestKnownField(Pair.Key, KnownFields);
			const FString Message = Suggestion.IsEmpty()
				? FString::Printf(TEXT("Unknown JSON field '%s' will be ignored."), *Pair.Key)
				: FString::Printf(TEXT("Unknown JSON field '%s' will be ignored. Did you mean '%s'?"), *Pair.Key, *Suggestion);
			AddIssue(Warnings, TEXT("warning"), TEXT("json_unknown_field"), Path + TEXT(".") + Pair.Key, Message);
		}
	}

	static const TSharedPtr<FJsonValue>* FindFieldValue(const TSharedPtr<FJsonObject>& Obj, const FString& FieldName)
	{
		return Obj.IsValid() ? Obj->Values.Find(FieldName) : nullptr;
	}

	static bool ReadStringField(
		const TSharedPtr<FJsonObject>& Obj,
		const FString& FieldName,
		const FString& Path,
		FString& OutValue,
		TArray<TSharedPtr<FJsonValue>>& Issues,
		const bool bRequired)
	{
		const TSharedPtr<FJsonValue>* FieldValue = FindFieldValue(Obj, FieldName);
		if (!FieldValue)
		{
			if (bRequired)
			{
				AddIssue(Issues, TEXT("warning"), TEXT("json_missing_required_field"), Path, TEXT("Required string field is missing."));
			}
			return false;
		}

		if (Obj->TryGetStringField(FieldName, OutValue))
		{
			return true;
		}

		AddIssue(
			Issues,
			TEXT("warning"),
			TEXT("json_field_type_mismatch"),
			Path,
			FString::Printf(TEXT("Expected string but got %s."), *JsonValueTypeToString(*FieldValue)));
		return false;
	}

	static bool ReadBoolField(
		const TSharedPtr<FJsonObject>& Obj,
		const FString& FieldName,
		const FString& Path,
		bool& bOutValue,
		TArray<TSharedPtr<FJsonValue>>& Issues,
		const bool bRequired)
	{
		const TSharedPtr<FJsonValue>* FieldValue = FindFieldValue(Obj, FieldName);
		if (!FieldValue)
		{
			if (bRequired)
			{
				AddIssue(Issues, TEXT("warning"), TEXT("json_missing_required_field"), Path, TEXT("Required bool field is missing."));
			}
			return false;
		}

		if (Obj->TryGetBoolField(FieldName, bOutValue))
		{
			return true;
		}

		AddIssue(
			Issues,
			TEXT("warning"),
			TEXT("json_field_type_mismatch"),
			Path,
			FString::Printf(TEXT("Expected bool but got %s."), *JsonValueTypeToString(*FieldValue)));
		return false;
	}

	static bool TryReadNumberValue(const TSharedPtr<FJsonValue>& Value, double& OutValue)
	{
		return Value.IsValid() && Value->TryGetNumber(OutValue);
	}

	static bool TryReadNumberFieldByAliases(
		const TSharedPtr<FJsonObject>& Obj,
		const TArray<FString>& FieldNames,
		double& OutValue)
	{
		if (!Obj.IsValid())
		{
			return false;
		}

		for (const FString& FieldName : FieldNames)
		{
			const TSharedPtr<FJsonValue>* FieldValue = Obj->Values.Find(FieldName);
			if (FieldValue && TryReadNumberValue(*FieldValue, OutValue))
			{
				return true;
			}
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Obj->Values)
		{
			for (const FString& FieldName : FieldNames)
			{
				if (Pair.Key.Equals(FieldName, ESearchCase::IgnoreCase) && TryReadNumberValue(Pair.Value, OutValue))
				{
					return true;
				}
			}
		}

		return false;
	}

	static bool TryReadVectorObject(const TSharedPtr<FJsonObject>& Obj, FVector& OutValue)
	{
		double X = 0.0;
		double Y = 0.0;
		double Z = 0.0;
		if (!TryReadNumberFieldByAliases(Obj, { TEXT("x"), TEXT("X") }, X)
			|| !TryReadNumberFieldByAliases(Obj, { TEXT("y"), TEXT("Y") }, Y)
			|| !TryReadNumberFieldByAliases(Obj, { TEXT("z"), TEXT("Z") }, Z))
		{
			return false;
		}

		OutValue = FVector(X, Y, Z);
		return true;
	}

	static bool TryReadRotatorObject(const TSharedPtr<FJsonObject>& Obj, FRotator& OutValue)
	{
		double Pitch = 0.0;
		double Yaw = 0.0;
		double Roll = 0.0;
		if (!TryReadNumberFieldByAliases(Obj, { TEXT("pitch"), TEXT("Pitch") }, Pitch)
			|| !TryReadNumberFieldByAliases(Obj, { TEXT("yaw"), TEXT("Yaw") }, Yaw)
			|| !TryReadNumberFieldByAliases(Obj, { TEXT("roll"), TEXT("Roll") }, Roll))
		{
			return false;
		}

		OutValue = FRotator(Pitch, Yaw, Roll);
		return true;
	}

	static bool TryReadVectorValue(const TSharedPtr<FJsonValue>& Value, FVector& OutValue)
	{
		const TSharedPtr<FJsonObject>* ObjectPtr = nullptr;
		if (Value.IsValid() && Value->TryGetObject(ObjectPtr) && ObjectPtr && ObjectPtr->IsValid())
		{
			return TryReadVectorObject(*ObjectPtr, OutValue);
		}

		const TArray<TSharedPtr<FJsonValue>>* ArrayPtr = nullptr;
		if (Value.IsValid() && Value->TryGetArray(ArrayPtr) && ArrayPtr && ArrayPtr->Num() >= 3)
		{
			double X = 0.0;
			double Y = 0.0;
			double Z = 0.0;
			if (TryReadNumberValue((*ArrayPtr)[0], X)
				&& TryReadNumberValue((*ArrayPtr)[1], Y)
				&& TryReadNumberValue((*ArrayPtr)[2], Z))
			{
				OutValue = FVector(X, Y, Z);
				return true;
			}
		}

		return false;
	}

	static bool TryReadRotatorValue(const TSharedPtr<FJsonValue>& Value, FRotator& OutValue)
	{
		const TSharedPtr<FJsonObject>* ObjectPtr = nullptr;
		if (Value.IsValid() && Value->TryGetObject(ObjectPtr) && ObjectPtr && ObjectPtr->IsValid())
		{
			return TryReadRotatorObject(*ObjectPtr, OutValue);
		}

		const TArray<TSharedPtr<FJsonValue>>* ArrayPtr = nullptr;
		if (Value.IsValid() && Value->TryGetArray(ArrayPtr) && ArrayPtr && ArrayPtr->Num() >= 3)
		{
			double Pitch = 0.0;
			double Yaw = 0.0;
			double Roll = 0.0;
			if (TryReadNumberValue((*ArrayPtr)[0], Pitch)
				&& TryReadNumberValue((*ArrayPtr)[1], Yaw)
				&& TryReadNumberValue((*ArrayPtr)[2], Roll))
			{
				OutValue = FRotator(Pitch, Yaw, Roll);
				return true;
			}
		}

		return false;
	}

	static bool ReadVectorField(
		const TSharedPtr<FJsonObject>& Obj,
		const FString& FieldName,
		const FString& Path,
		FVector& OutValue,
		TArray<TSharedPtr<FJsonValue>>& Issues,
		const bool bRequired)
	{
		const TSharedPtr<FJsonValue>* FieldValue = FindFieldValue(Obj, FieldName);
		if (!FieldValue)
		{
			if (bRequired)
			{
				AddIssue(Issues, TEXT("warning"), TEXT("json_missing_required_field"), Path, TEXT("Required vector field is missing."));
			}
			return false;
		}

		if (TryReadVectorValue(*FieldValue, OutValue))
		{
			return true;
		}

		AddIssue(
			Issues,
			TEXT("warning"),
			TEXT("json_field_type_mismatch"),
			Path,
			FString::Printf(TEXT("Expected vector object {x,y,z}/{X,Y,Z} or numeric array [x,y,z] but got %s."), *JsonValueTypeToString(*FieldValue)));
		return false;
	}

	static bool ReadRotatorField(
		const TSharedPtr<FJsonObject>& Obj,
		const FString& FieldName,
		const FString& Path,
		FRotator& OutValue,
		TArray<TSharedPtr<FJsonValue>>& Issues,
		const bool bRequired)
	{
		const TSharedPtr<FJsonValue>* FieldValue = FindFieldValue(Obj, FieldName);
		if (!FieldValue)
		{
			if (bRequired)
			{
				AddIssue(Issues, TEXT("warning"), TEXT("json_missing_required_field"), Path, TEXT("Required rotator field is missing."));
			}
			return false;
		}

		if (TryReadRotatorValue(*FieldValue, OutValue))
		{
			return true;
		}

		AddIssue(
			Issues,
			TEXT("warning"),
			TEXT("json_field_type_mismatch"),
			Path,
			FString::Printf(TEXT("Expected rotator object {pitch,yaw,roll}/{Pitch,Yaw,Roll} or numeric array [pitch,yaw,roll] but got %s."), *JsonValueTypeToString(*FieldValue)));
		return false;
	}

	static bool ReadArrayField(
		const TSharedPtr<FJsonObject>& Obj,
		const FString& FieldName,
		const FString& Path,
		const TArray<TSharedPtr<FJsonValue>>*& OutArray,
		TArray<TSharedPtr<FJsonValue>>& Issues,
		const bool bRequired)
	{
		OutArray = nullptr;
		const TSharedPtr<FJsonValue>* FieldValue = FindFieldValue(Obj, FieldName);
		if (!FieldValue)
		{
			if (bRequired)
			{
				AddIssue(Issues, TEXT("warning"), TEXT("json_missing_required_field"), Path, TEXT("Required array field is missing."));
			}
			return false;
		}

		if (Obj->TryGetArrayField(FieldName, OutArray) && OutArray != nullptr)
		{
			return true;
		}

		AddIssue(
			Issues,
			TEXT("warning"),
			TEXT("json_field_type_mismatch"),
			Path,
			FString::Printf(TEXT("Expected array but got %s."), *JsonValueTypeToString(*FieldValue)));
		return false;
	}

	static bool ReadObjectFromValue(
		const TSharedPtr<FJsonValue>& Value,
		const FString& Path,
		TSharedPtr<FJsonObject>& OutObject,
		TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		const TSharedPtr<FJsonObject>* ObjectPtr = nullptr;
		if (Value.IsValid() && Value->TryGetObject(ObjectPtr) && ObjectPtr && ObjectPtr->IsValid())
		{
			OutObject = *ObjectPtr;
			return true;
		}

		AddIssue(
			Issues,
			TEXT("warning"),
			TEXT("json_array_item_type_mismatch"),
			Path,
			FString::Printf(TEXT("Expected object array item but got %s."), *JsonValueTypeToString(Value)));
		return false;
	}

	static bool LoadJsonObjectFile(
		const FString& FilePath,
		TSharedPtr<FJsonObject>& OutObj,
		TArray<TSharedPtr<FJsonValue>>* OutIssues,
		FString& OutError,
		const bool bOptional)
	{
		OutObj.Reset();
		const FString ResolvedFilePath = ResolveProjectRelativeFilePath(FilePath);
		if (bOptional && !FPaths::FileExists(ResolvedFilePath))
		{
			return true;
		}

		FString JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, *ResolvedFilePath))
		{
			OutError = FString::Printf(TEXT("json_file_not_found:%s"), *ResolvedFilePath);
			if (OutIssues)
			{
				AddIssue(*OutIssues, TEXT("error"), TEXT("json_file_not_found"), ResolvedFilePath, TEXT("JSON file does not exist or cannot be read."));
			}
			return false;
		}

		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(JsonText);
		if (!FJsonSerializer::Deserialize(Reader, OutObj) || !OutObj.IsValid())
		{
			OutError = FString::Printf(TEXT("json_parse_failed:%s"), *ResolvedFilePath);
			if (OutIssues)
			{
				AddIssue(*OutIssues, TEXT("error"), TEXT("json_parse_failed"), ResolvedFilePath, TEXT("JSON syntax parse failed."));
			}
			return false;
		}

		return true;
	}
}

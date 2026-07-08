// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/UeAgentJsonDiagnostics.h"

#include "Utilities/UeAgentStructuredJsonFormatter.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace UeAgentJsonDiagnostics
{
	FString NormalizeResolvedFilePath(FString FilePath)
	{
		FPaths::NormalizeFilename(FilePath);
		FPaths::CollapseRelativeDirectories(FilePath);
		if (FilePath.Len() >= 3 && FilePath[1] == TEXT(':') && FilePath[2] == TEXT('/'))
		{
			const FString Prefix = FilePath.Left(3);
			TArray<FString> Parts;
			FilePath.RightChop(3).ParseIntoArray(Parts, TEXT("/"), true);
			TArray<FString> CollapsedParts;
			for (const FString& Part : Parts)
			{
				if (Part == TEXT("."))
				{
					continue;
				}
				if (Part == TEXT(".."))
				{
					if (CollapsedParts.Num() > 0)
					{
						CollapsedParts.Pop(EAllowShrinking::No);
					}
					continue;
				}
				CollapsedParts.Add(Part);
			}
			FilePath = Prefix + FString::Join(CollapsedParts, TEXT("/"));
		}
		return FilePath;
	}

	FString ResolveProjectRelativeFilePath(const FString& FilePathInput)
	{
		FString FilePath = FilePathInput;
		FilePath.TrimStartAndEndInline();
		if (FilePath.IsEmpty())
		{
			return FilePath;
		}
		FString ResolvedPath;
		if (FPaths::IsRelative(FilePath))
		{
			FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
			FPaths::NormalizeDirectoryName(ProjectDir);
			FPaths::CollapseRelativeDirectories(ProjectDir);
			ResolvedPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(ProjectDir, FilePath));
		}
		else
		{
			ResolvedPath = FPaths::ConvertRelativePathToFull(FilePath);
		}
		return NormalizeResolvedFilePath(ResolvedPath);
	}

	FString JsonValueTypeToString(const TSharedPtr<FJsonValue>& Value)
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

	TSharedPtr<FJsonObject> MakeIssue(
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

	void AddIssue(
		TArray<TSharedPtr<FJsonValue>>& Issues,
		const FString& Severity,
		const FString& Code,
		const FString& Path,
		const FString& Message)
	{
		Issues.Add(MakeShared<FJsonValueObject>(MakeIssue(Severity, Code, Path, Message)));
	}

	int32 CountIssuesBySeverity(
		const TArray<TSharedPtr<FJsonValue>>& Issues,
		const FString& Severity)
	{
		int32 Count = 0;
		for (const TSharedPtr<FJsonValue>& IssueValue : Issues)
		{
			const TSharedPtr<FJsonObject>* IssueObj = nullptr;
			FString IssueSeverity;
			if (IssueValue.IsValid()
				&& IssueValue->TryGetObject(IssueObj)
				&& IssueObj
				&& IssueObj->IsValid()
				&& (*IssueObj)->TryGetStringField(TEXT("severity"), IssueSeverity)
				&& IssueSeverity.Equals(Severity, ESearchCase::IgnoreCase))
			{
				++Count;
			}
		}
		return Count;
	}

	bool HasIssueWithSeverity(
		const TArray<TSharedPtr<FJsonValue>>& Issues,
		const FString& Severity)
	{
		return CountIssuesBySeverity(Issues, Severity) > 0;
	}

	bool HasErrorIssue(const TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		return HasIssueWithSeverity(Issues, TEXT("error"));
	}

	bool JsonObjectHasExplicitWriteIntent(const TSharedPtr<FJsonObject>& Obj)
	{
		if (!Obj.IsValid())
		{
			return false;
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Obj->Values)
		{
			const FString KeyLower = Pair.Key.ToLower();
			if (KeyLower == TEXT("apply")
				|| KeyLower == TEXT("remove")
				|| KeyLower == TEXT("delete")
				|| KeyLower == TEXT("set_default_profile")
				|| KeyLower == TEXT("write_intent"))
			{
				bool bValue = false;
				if (Pair.Value.IsValid() && Pair.Value->TryGetBool(bValue) && bValue)
				{
					return true;
				}
			}
			if (KeyLower == TEXT("rename")
				|| KeyLower == TEXT("rename_to")
				|| KeyLower == TEXT("operation"))
			{
				FString StringValue;
				if (Pair.Value.IsValid() && Pair.Value->TryGetString(StringValue) && !StringValue.TrimStartAndEnd().IsEmpty())
				{
					return true;
				}
			}
			if (KeyLower == TEXT("operations")
				|| KeyLower == TEXT("mutations")
				|| KeyLower == TEXT("write_operations")
				|| KeyLower == TEXT("import_profile"))
			{
				const TArray<TSharedPtr<FJsonValue>>* ArrayValue = nullptr;
				if (Pair.Value.IsValid() && Pair.Value->TryGetArray(ArrayValue) && ArrayValue && ArrayValue->Num() > 0)
				{
					return true;
				}
			}
			if (JsonValueHasExplicitWriteIntent(Pair.Value))
			{
				return true;
			}
		}
		return false;
	}

	bool JsonValueHasExplicitWriteIntent(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
		{
			return false;
		}
		const TSharedPtr<FJsonObject>* ObjectValue = nullptr;
		if (Value->TryGetObject(ObjectValue) && ObjectValue && ObjectValue->IsValid())
		{
			return JsonObjectHasExplicitWriteIntent(*ObjectValue);
		}
		const TArray<TSharedPtr<FJsonValue>>* ArrayValue = nullptr;
		if (Value->TryGetArray(ArrayValue) && ArrayValue)
		{
			for (const TSharedPtr<FJsonValue>& Item : *ArrayValue)
			{
				if (JsonValueHasExplicitWriteIntent(Item))
				{
					return true;
				}
			}
		}
		return false;
	}

	void AddUnsupportedApplyIssueIfWriteIntent(
		const TSharedPtr<FJsonObject>& Obj,
		const FString& Path,
		const FString& ProfileName,
		TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		if (JsonObjectHasExplicitWriteIntent(Obj))
		{
			AddIssue(
				Issues,
				TEXT("error"),
				TEXT("unsupported_apply_profile"),
				Path,
				FString::Printf(TEXT("%s is read-only or summary-only in this folder workflow. Use the documented safe JSON file or explicit command for this write."), *ProfileName));
		}
	}

	bool SerializeJsonObjectCompact(const TSharedPtr<FJsonObject>& Obj, FString& OutText)
	{
		if (!Obj.IsValid())
		{
			OutText.Reset();
			return false;
		}
		OutText = FUeAgentStructuredJsonFormatter::WriteObject(Obj.ToSharedRef());
		return true;
	}

	bool SerializeJsonValueCompact(const TSharedPtr<FJsonValue>& Value, FString& OutText)
	{
		if (!Value.IsValid())
		{
			OutText = TEXT("null");
			return true;
		}
		OutText = FUeAgentStructuredJsonFormatter::WriteValue(Value.ToSharedRef());
		return true;
	}

	FString SerializeJsonArrayCompact(const TArray<TSharedPtr<FJsonValue>>& Values)
	{
		TSharedPtr<FJsonValue> ArrayValue = MakeShared<FJsonValueArray>(Values);
		FString JsonText;
		SerializeJsonValueCompact(ArrayValue, JsonText);
		return JsonText;
	}

	int32 EditDistance(const FString& AInput, const FString& BInput)
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

	FString FindClosestKnownField(const FString& FieldName, const TArray<FString>& KnownFields)
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

	void WarnUnknownFields(
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

	const TSharedPtr<FJsonValue>* FindFieldValue(const TSharedPtr<FJsonObject>& Obj, const FString& FieldName)
	{
		return Obj.IsValid() ? Obj->Values.Find(FieldName) : nullptr;
	}

	bool ReadStringField(
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

	bool ReadBoolField(
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

	bool TryReadNumberValue(const TSharedPtr<FJsonValue>& Value, double& OutValue)
	{
		return Value.IsValid() && Value->TryGetNumber(OutValue);
	}

	bool TryReadNumberFieldByAliases(
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

	bool TryReadVectorObject(const TSharedPtr<FJsonObject>& Obj, FVector& OutValue)
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

	bool TryReadRotatorObject(const TSharedPtr<FJsonObject>& Obj, FRotator& OutValue)
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

	bool TryReadVectorValue(const TSharedPtr<FJsonValue>& Value, FVector& OutValue)
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

	bool TryReadVector2DValue(const TSharedPtr<FJsonValue>& Value, FVector2D& OutValue)
	{
		const TSharedPtr<FJsonObject>* ObjectPtr = nullptr;
		if (Value.IsValid() && Value->TryGetObject(ObjectPtr) && ObjectPtr && ObjectPtr->IsValid())
		{
			double X = 0.0;
			double Y = 0.0;
			if (TryReadNumberFieldByAliases(*ObjectPtr, { TEXT("x"), TEXT("X") }, X)
				&& TryReadNumberFieldByAliases(*ObjectPtr, { TEXT("y"), TEXT("Y") }, Y))
			{
				OutValue = FVector2D(X, Y);
				return true;
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* ArrayPtr = nullptr;
		if (Value.IsValid() && Value->TryGetArray(ArrayPtr) && ArrayPtr && ArrayPtr->Num() >= 2)
		{
			double X = 0.0;
			double Y = 0.0;
			if (TryReadNumberValue((*ArrayPtr)[0], X)
				&& TryReadNumberValue((*ArrayPtr)[1], Y))
			{
				OutValue = FVector2D(X, Y);
				return true;
			}
		}

		return false;
	}

	bool TryReadRotatorValue(const TSharedPtr<FJsonValue>& Value, FRotator& OutValue)
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

	bool TryReadLinearColorValue(const TSharedPtr<FJsonValue>& Value, FLinearColor& OutValue)
	{
		const TSharedPtr<FJsonObject>* ObjectPtr = nullptr;
		if (Value.IsValid() && Value->TryGetObject(ObjectPtr) && ObjectPtr && ObjectPtr->IsValid())
		{
			double R = 0.0;
			double G = 0.0;
			double B = 0.0;
			double A = 1.0;
			const bool bHasRgb =
				TryReadNumberFieldByAliases(*ObjectPtr, { TEXT("r"), TEXT("R"), TEXT("red"), TEXT("Red"), TEXT("x"), TEXT("X") }, R)
				&& TryReadNumberFieldByAliases(*ObjectPtr, { TEXT("g"), TEXT("G"), TEXT("green"), TEXT("Green"), TEXT("y"), TEXT("Y") }, G)
				&& TryReadNumberFieldByAliases(*ObjectPtr, { TEXT("b"), TEXT("B"), TEXT("blue"), TEXT("Blue"), TEXT("z"), TEXT("Z") }, B);
			if (bHasRgb)
			{
				TryReadNumberFieldByAliases(*ObjectPtr, { TEXT("a"), TEXT("A"), TEXT("alpha"), TEXT("Alpha"), TEXT("w"), TEXT("W") }, A);
				OutValue = FLinearColor((float)R, (float)G, (float)B, (float)A);
				return true;
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* ArrayPtr = nullptr;
		if (Value.IsValid() && Value->TryGetArray(ArrayPtr) && ArrayPtr && ArrayPtr->Num() >= 3)
		{
			double R = 0.0;
			double G = 0.0;
			double B = 0.0;
			double A = 1.0;
			if (TryReadNumberValue((*ArrayPtr)[0], R)
				&& TryReadNumberValue((*ArrayPtr)[1], G)
				&& TryReadNumberValue((*ArrayPtr)[2], B))
			{
				if (ArrayPtr->Num() >= 4)
				{
					TryReadNumberValue((*ArrayPtr)[3], A);
				}
				OutValue = FLinearColor((float)R, (float)G, (float)B, (float)A);
				return true;
			}
		}

		return false;
	}

	bool ReadVectorField(
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

	bool ReadRotatorField(
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

	bool ReadArrayField(
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

	bool ReadObjectFromValue(
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

	bool LoadJsonObjectFile(
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

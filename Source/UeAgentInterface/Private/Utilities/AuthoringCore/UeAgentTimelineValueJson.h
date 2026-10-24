#pragma once

#include "Utilities/AuthoringCore/UeAgentTimelineValueTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

namespace UE::UAI::Authoring::Timeline::Json
{
	inline bool Fail(UE::UAI::Contract::FDiagnostic& OutError, UE::UAI::Contract::FDiagnostic Diagnostic, const FString& SubjectId = FString())
    { OutError = MoveTemp(Diagnostic); if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = SubjectId; return false; }

	inline bool ValueFailure(const TSharedPtr<FJsonObject>& Root, const FString& Pointer,
		const FString& Code, const FString& Expected, const TSharedPtr<FJsonValue>& Actual,
		FUeAgentTimelineAnimationExecutionError& Error)
	{
		Error = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Pointer,
			MakeShared<FJsonValueString>(Expected), Actual, TEXT("timeline_decode"),
			TEXT("The timeline value must satisfy: ") + Expected + TEXT("."));
		if (Root) UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Root);
		return false;
	}

	inline bool FieldFailure(const TSharedPtr<FJsonObject>& Root, const TCHAR* Field,
		const FString& Code, const FString& Expected, FUeAgentTimelineAnimationExecutionError& Error)
	{
		return ValueFailure(Root, TEXT("/") + FString(Field).Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")),
			Code, Expected, Root ? Root->TryGetField(Field) : nullptr, Error);
	}

	inline bool PrefixChildFailure(const TSharedPtr<FJsonObject>& Root, const FString& Pointer,
		FUeAgentTimelineAnimationExecutionError& Error)
	{
		// A resolved source already identifies the original file and full path.
		// Only relative, unbound diagnostics need the containing field/index.
		if (Error.Source.SourceFile.IsEmpty())
		{
			UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error, Pointer);
			if (Root) UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Root);
		}
		return false;
	}

	template <typename NumberType>
	bool RequiredNumber(const TSharedPtr<FJsonObject>& Root, const TCHAR* Field,
		NumberType& Out, FUeAgentTimelineAnimationExecutionError& Error)
	{
		if (Root && Root->TryGetNumberField(Field, Out) && FMath::IsFinite(static_cast<double>(Out))) return true;
		return FieldFailure(Root, Field, TEXT("timeline_animation_field_number_invalid"), TEXT("a finite number"), Error);
	}


    

	inline bool RequiredObject(
		const TSharedPtr<FJsonObject>& Root,
		const TCHAR* Field,
		TSharedPtr<FJsonObject>& Out,
		FUeAgentTimelineAnimationExecutionError& Error)
	{
		const TSharedPtr<FJsonObject>* Found = nullptr;
		if (!Root.IsValid() || !Root->TryGetObjectField(Field, Found)
			|| !Found || !Found->IsValid())
		{
			return FieldFailure(Root, Field, TEXT("timeline_animation_field_object_missing"), TEXT("an object"), Error);
		}
		Out = *Found;
		return true;
	}

	inline bool OptionalObject(
		const TSharedPtr<FJsonObject>& Root,
		const TCHAR* Field,
		TSharedPtr<FJsonObject>& Out,
		bool& bPresent,
		FUeAgentTimelineAnimationExecutionError& Error)
	{
		Out.Reset();
		bPresent = Root.IsValid() && Root->HasField(Field);
		return !bPresent || RequiredObject(Root, Field, Out, Error);
	}

	inline bool RequiredString(
		const TSharedPtr<FJsonObject>& Root,
		const TCHAR* Field,
		FString& Out,
		FUeAgentTimelineAnimationExecutionError& Error)
	{
		if (!Root.IsValid() || !Root->TryGetStringField(Field, Out))
		{
			return FieldFailure(Root, Field, TEXT("timeline_animation_field_string_missing"), TEXT("a string"), Error);
		}
		return true;
	}

	inline bool OptionalString(
		const TSharedPtr<FJsonObject>& Root,
		const TCHAR* Field,
		FString& Out,
		FUeAgentTimelineAnimationExecutionError& Error)
	{
		Out.Reset();
		return !Root->HasField(Field) || RequiredString(Root, Field, Out, Error);
	}

	inline bool OptionalBool(
		const TSharedPtr<FJsonObject>& Root,
		const TCHAR* Field,
		TOptional<bool>& Out,
		FUeAgentTimelineAnimationExecutionError& Error)
	{
		Out.Reset();
		if (!Root->HasField(Field))
		{
			return true;
		}
		bool Value = false;
		if (!Root->TryGetBoolField(Field, Value))
		{
			return FieldFailure(Root, Field, TEXT("timeline_animation_field_boolean_invalid"), TEXT("a boolean"), Error);
		}
		Out = Value;
		return true;
	}

	inline bool OptionalNumber(
		const TSharedPtr<FJsonObject>& Root,
		const TCHAR* Field,
		TOptional<double>& Out,
		FUeAgentTimelineAnimationExecutionError& Error)
	{
		Out.Reset();
		if (!Root->HasField(Field))
		{
			return true;
		}
		double Value = 0.0;
		if (!Root->TryGetNumberField(Field, Value) || !FMath::IsFinite(Value))
		{
			return FieldFailure(Root, Field, TEXT("timeline_animation_field_number_invalid"), TEXT("a finite number"), Error);
		}
		Out = Value;
		return true;
	}

	inline bool ParseStringArray(
		const TSharedPtr<FJsonObject>& Root,
		const TCHAR* Field,
		TArray<FString>& Out,
		bool bRequired,
		FUeAgentTimelineAnimationExecutionError& Error)
	{
		Out.Reset();
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!bRequired && !Root->HasField(Field)) return true;
		if (!Root->TryGetArrayField(Field, Values) || !Values)
		{
			return FieldFailure(Root, Field, TEXT("timeline_animation_field_array_missing"), TEXT("an array"), Error);
		}
		for (int32 Index = 0; Index < Values->Num(); ++Index)
		{
			const auto& Value = (*Values)[Index];
			FString Text;
			if (!Value.IsValid() || !Value->TryGetString(Text))
			{
				return ValueFailure(Root, FString::Printf(TEXT("/%s/%d"), Field, Index), TEXT("timeline_animation_array_string_invalid"), TEXT("a string"), Value, Error);
			}
			Out.Add(MoveTemp(Text));
		}
		return true;
	}

	template <typename ItemType, typename ParseItemType>
	bool ParseObjectArray(
		const TSharedPtr<FJsonObject>& Root,
		const TCHAR* Field,
		TArray<ItemType>& Out,
		bool bRequired,
		ParseItemType ParseItem,
		FUeAgentTimelineAnimationExecutionError& Error)
	{
		Out.Reset();
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!bRequired && !Root->HasField(Field)) return true;
		if (!Root->TryGetArrayField(Field, Values) || !Values)
		{
			return FieldFailure(Root, Field, TEXT("timeline_animation_field_array_missing"), TEXT("an array"), Error);
		}
		for (int32 Index = 0; Index < Values->Num(); ++Index)
		{
			const auto& Value = (*Values)[Index];
			const FString Pointer = FString::Printf(TEXT("/%s/%d"), Field, Index);
			const TSharedPtr<FJsonObject> Object =
				Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
			ItemType Item;
			if (!Object) return ValueFailure(Root, Pointer, TEXT("timeline_animation_array_object_invalid"), TEXT("an object"), Value, Error);
			if (!ParseItem(Object, Item, Error))
			{
				if (Error.IsEmpty()) ValueFailure(Root, Pointer, TEXT("timeline_animation_array_item_invalid"), TEXT("a valid timeline item"), Value, Error);
				else PrefixChildFailure(Root, Pointer, Error);
				return false;
			}
			Out.Add(MoveTemp(Item));
		}
		return true;
	}
}

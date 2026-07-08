#include "Utilities/UeAgentStructuredJsonFormatter.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
	static FString UeAgentEscapeJsonString(const FString& Value)
	{
		FString Out;
		Out.Reserve(Value.Len() + 2);
		Out.AppendChar(TEXT('"'));
		for (const TCHAR Ch : Value)
		{
			switch (Ch)
			{
			case TEXT('"'):
				Out += TEXT("\\\"");
				break;
			case TEXT('\\'):
				Out += TEXT("\\\\");
				break;
			case TEXT('\b'):
				Out += TEXT("\\b");
				break;
			case TEXT('\f'):
				Out += TEXT("\\f");
				break;
			case TEXT('\n'):
				Out += TEXT("\\n");
				break;
			case TEXT('\r'):
				Out += TEXT("\\r");
				break;
			case TEXT('\t'):
				Out += TEXT("\\t");
				break;
			default:
				if (Ch < 0x20)
				{
					Out += FString::Printf(TEXT("\\u%04x"), static_cast<uint32>(Ch));
				}
				else
				{
					Out.AppendChar(Ch);
				}
				break;
			}
		}
		Out.AppendChar(TEXT('"'));
		return Out;
	}

	static FString FormatJsonNumber(double Value)
	{
		if (FMath::IsFinite(Value) && FMath::IsNearlyEqual(Value, FMath::RoundToDouble(Value)))
		{
			return FString::Printf(TEXT("%.0f"), Value);
		}
		return FString::SanitizeFloat(Value);
	}

	static bool IsScalarJsonValue(const TSharedPtr<FJsonValue>& Value)
	{
		return !Value.IsValid()
			|| Value->Type == EJson::None
			|| Value->Type == EJson::Null
			|| Value->Type == EJson::String
			|| Value->Type == EJson::Number
			|| Value->Type == EJson::Boolean;
	}
}

FUeAgentStructuredJsonFormatOptions::FUeAgentStructuredJsonFormatOptions()
	: Indent(2)
	, MaxLineLength(140)
	, MaxExposedProperties(6)
	, MaxInlineDepth(2)
	, bBreakOnArrayFields(true)
{
}

FString FUeAgentStructuredJsonFormatter::WriteObject(
	const TSharedRef<FJsonObject>& Object,
	const FUeAgentStructuredJsonFormatOptions& Options)
{
	FUeAgentStructuredJsonFormatter Formatter(Options);
	return Formatter.FormatObject(Object, 0);
}

FString FUeAgentStructuredJsonFormatter::WriteValue(
	const TSharedRef<FJsonValue>& Value,
	const FUeAgentStructuredJsonFormatOptions& Options)
{
	FUeAgentStructuredJsonFormatter Formatter(Options);
	return Formatter.FormatValue(Value, 0);
}

bool FUeAgentStructuredJsonFormatter::SaveObjectToFile(
	const FString& FilePath,
	const TSharedPtr<FJsonObject>& Object,
	FString* OutError,
	const FUeAgentStructuredJsonFormatOptions& Options)
{
	if (!Object.IsValid())
	{
		if (OutError)
		{
			*OutError = TEXT("json_object_invalid");
		}
		return false;
	}

	const FString Directory = FPaths::GetPath(FilePath);
	if (!Directory.IsEmpty())
	{
		IFileManager::Get().MakeDirectory(*Directory, true);
	}

	const FString Text = WriteObject(Object.ToSharedRef(), Options) + LINE_TERMINATOR;
	if (!FFileHelper::SaveStringToFile(Text, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		if (OutError)
		{
			*OutError = FString::Printf(TEXT("write_failed:%s"), *FilePath);
		}
		return false;
	}
	return true;
}

bool FUeAgentStructuredJsonFormatter::SaveValueToFile(
	const FString& FilePath,
	const TSharedPtr<FJsonValue>& Value,
	FString* OutError,
	const FUeAgentStructuredJsonFormatOptions& Options)
{
	if (!Value.IsValid())
	{
		if (OutError)
		{
			*OutError = TEXT("json_value_invalid");
		}
		return false;
	}

	const FString Directory = FPaths::GetPath(FilePath);
	if (!Directory.IsEmpty())
	{
		IFileManager::Get().MakeDirectory(*Directory, true);
	}

	const FString Text = WriteValue(Value.ToSharedRef(), Options) + LINE_TERMINATOR;
	if (!FFileHelper::SaveStringToFile(Text, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		if (OutError)
		{
			*OutError = FString::Printf(TEXT("write_failed:%s"), *FilePath);
		}
		return false;
	}
	return true;
}

FUeAgentStructuredJsonFormatter::FUeAgentStructuredJsonFormatter(const FUeAgentStructuredJsonFormatOptions& InOptions)
	: Options(InOptions)
{
}

FString FUeAgentStructuredJsonFormatter::FormatValue(const TSharedPtr<FJsonValue>& Value, int32 Level) const
{
	if (CanInlineValue(Value, Level, 0))
	{
		return InlineValue(Value);
	}
	if (Value.IsValid() && Value->Type == EJson::Object)
	{
		return FormatObject(Value->AsObject(), Level);
	}
	if (Value.IsValid() && Value->Type == EJson::Array)
	{
		return FormatArray(Value->AsArray(), Level);
	}
	return InlineValue(Value);
}

FString FUeAgentStructuredJsonFormatter::FormatObject(const TSharedPtr<FJsonObject>& Object, int32 Level) const
{
	if (!Object.IsValid() || Object->Values.Num() == 0)
	{
		return TEXT("{}");
	}

	TSharedPtr<FJsonValueObject> ObjectValue = MakeShared<FJsonValueObject>(Object);
	if (CanInlineValue(ObjectValue, Level, 0))
	{
		return InlineValue(ObjectValue);
	}

	FString Out = TEXT("{");
	int32 Index = 0;
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
	{
		const bool bHasMore = Index < Object->Values.Num() - 1;
		Out += TEXT("\n");
		Out += Padding(Level + 1);
		Out += UeAgentEscapeJsonString(Pair.Key);
		Out += TEXT(": ");
		Out += FormatValue(Pair.Value, Level + 1);
		if (bHasMore)
		{
			Out += TEXT(",");
		}
		++Index;
	}
	Out += TEXT("\n");
	Out += Padding(Level);
	Out += TEXT("}");
	return Out;
}

FString FUeAgentStructuredJsonFormatter::FormatArray(const TArray<TSharedPtr<FJsonValue>>& Values, int32 Level) const
{
	if (Values.Num() == 0)
	{
		return TEXT("[]");
	}

	TSharedPtr<FJsonValueArray> ArrayValue = MakeShared<FJsonValueArray>(Values);
	if (CanInlineValue(ArrayValue, Level, 0))
	{
		return InlineValue(ArrayValue);
	}

	FString Out = TEXT("[");
	for (int32 Index = 0; Index < Values.Num(); ++Index)
	{
		Out += TEXT("\n");
		Out += Padding(Level + 1);
		Out += FormatValue(Values[Index], Level + 1);
		if (Index < Values.Num() - 1)
		{
			Out += TEXT(",");
		}
	}
	Out += TEXT("\n");
	Out += Padding(Level);
	Out += TEXT("]");
	return Out;
}

bool FUeAgentStructuredJsonFormatter::CanInlineValue(const TSharedPtr<FJsonValue>& Value, int32 Level, int32 Depth) const
{
	if (IsScalarJsonValue(Value))
	{
		return FitsInline(Value, Level);
	}

	if (Value->Type == EJson::Object)
	{
		const TSharedPtr<FJsonObject> Object = Value->AsObject();
		if (!Object.IsValid() || Object->Values.Num() == 0)
		{
			return true;
		}
		if (Depth >= Options.MaxInlineDepth)
		{
			return false;
		}
		if (Options.bBreakOnArrayFields && HasExposedArrayProperty(Object))
		{
			return false;
		}
		if (CountExposedProperties(Value) > Options.MaxExposedProperties)
		{
			return false;
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
		{
			if (!CanInlineValue(Pair.Value, Level, Depth + 1))
			{
				return false;
			}
		}
		return FitsInline(Value, Level);
	}

	if (Value->Type == EJson::Array)
	{
		const TArray<TSharedPtr<FJsonValue>>& Values = Value->AsArray();
		if (Values.Num() == 0)
		{
			return true;
		}
		for (const TSharedPtr<FJsonValue>& Child : Values)
		{
			if (!IsScalarJsonValue(Child))
			{
				return false;
			}
		}
		return FitsInline(Value, Level);
	}

	return FitsInline(Value, Level);
}

bool FUeAgentStructuredJsonFormatter::FitsInline(const TSharedPtr<FJsonValue>& Value, int32 Level) const
{
	const FString Inline = InlineValue(Value);
	return !Inline.Contains(TEXT("\n")) && Padding(Level).Len() + Inline.Len() <= Options.MaxLineLength;
}

bool FUeAgentStructuredJsonFormatter::HasExposedArrayProperty(const TSharedPtr<FJsonObject>& Object) const
{
	if (!Object.IsValid())
	{
		return false;
	}

	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
	{
		if (Pair.Value.IsValid() && Pair.Value->Type == EJson::Array)
		{
			return true;
		}
		if (Pair.Value.IsValid() && Pair.Value->Type == EJson::Object && HasExposedArrayProperty(Pair.Value->AsObject()))
		{
			return true;
		}
	}
	return false;
}

int32 FUeAgentStructuredJsonFormatter::CountExposedProperties(const TSharedPtr<FJsonValue>& Value) const
{
	if (!Value.IsValid() || Value->Type == EJson::None || Value->Type == EJson::Null)
	{
		return 1;
	}
	if (Value->Type == EJson::Object)
	{
		int32 Total = 0;
		const TSharedPtr<FJsonObject> Object = Value->AsObject();
		if (!Object.IsValid())
		{
			return 1;
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
		{
			if (Pair.Value.IsValid() && Pair.Value->Type == EJson::Object)
			{
				Total += CountExposedProperties(Pair.Value);
			}
			else
			{
				++Total;
			}
		}
		return Total;
	}
	return 1;
}

FString FUeAgentStructuredJsonFormatter::InlineValue(const TSharedPtr<FJsonValue>& Value) const
{
	if (!Value.IsValid() || Value->Type == EJson::None || Value->Type == EJson::Null)
	{
		return TEXT("null");
	}

	switch (Value->Type)
	{
	case EJson::String:
		return UeAgentEscapeJsonString(Value->AsString());
	case EJson::Number:
		return FormatJsonNumber(Value->AsNumber());
	case EJson::Boolean:
		return Value->AsBool() ? TEXT("true") : TEXT("false");
	case EJson::Array:
	{
		const TArray<TSharedPtr<FJsonValue>>& Values = Value->AsArray();
		FString Out = TEXT("[");
		for (int32 Index = 0; Index < Values.Num(); ++Index)
		{
			if (Index > 0)
			{
				Out += TEXT(", ");
			}
			Out += InlineValue(Values[Index]);
		}
		Out += TEXT("]");
		return Out;
	}
	case EJson::Object:
	{
		const TSharedPtr<FJsonObject> Object = Value->AsObject();
		if (!Object.IsValid())
		{
			return TEXT("{}");
		}
		FString Out = TEXT("{");
		int32 Index = 0;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
		{
			if (Index > 0)
			{
				Out += TEXT(", ");
			}
			Out += UeAgentEscapeJsonString(Pair.Key);
			Out += TEXT(": ");
			Out += InlineValue(Pair.Value);
			++Index;
		}
		Out += TEXT("}");
		return Out;
	}
	default:
		return TEXT("null");
	}
}

FString FUeAgentStructuredJsonFormatter::Padding(int32 Level) const
{
	return FString::ChrN(FMath::Max(0, Level * Options.Indent), TEXT(' '));
}

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

struct UEAGENTINTERFACE_API FUeAgentStructuredJsonFormatOptions
{
	FUeAgentStructuredJsonFormatOptions();

	int32 Indent;
	int32 MaxLineLength;
	int32 MaxExposedProperties;
	int32 MaxInlineDepth;
	bool bBreakOnArrayFields;
};

class UEAGENTINTERFACE_API FUeAgentStructuredJsonFormatter
{
public:
	static FString WriteObject(
		const TSharedRef<FJsonObject>& Object,
		const FUeAgentStructuredJsonFormatOptions& Options = FUeAgentStructuredJsonFormatOptions());

	static FString WriteValue(
		const TSharedRef<FJsonValue>& Value,
		const FUeAgentStructuredJsonFormatOptions& Options = FUeAgentStructuredJsonFormatOptions());

	static bool SaveObjectToFile(
		const FString& FilePath,
		const TSharedPtr<FJsonObject>& Object,
		FString* OutError = nullptr,
		const FUeAgentStructuredJsonFormatOptions& Options = FUeAgentStructuredJsonFormatOptions());

	static bool SaveValueToFile(
		const FString& FilePath,
		const TSharedPtr<FJsonValue>& Value,
		FString* OutError = nullptr,
		const FUeAgentStructuredJsonFormatOptions& Options = FUeAgentStructuredJsonFormatOptions());

private:
	explicit FUeAgentStructuredJsonFormatter(const FUeAgentStructuredJsonFormatOptions& InOptions);

	FString FormatValue(const TSharedPtr<FJsonValue>& Value, int32 Level) const;
	FString FormatObject(const TSharedPtr<FJsonObject>& Object, int32 Level) const;
	FString FormatArray(const TArray<TSharedPtr<FJsonValue>>& Values, int32 Level) const;
	bool CanInlineValue(const TSharedPtr<FJsonValue>& Value, int32 Level, int32 Depth) const;
	bool FitsInline(const TSharedPtr<FJsonValue>& Value, int32 Level) const;
	bool HasExposedArrayProperty(const TSharedPtr<FJsonObject>& Object) const;
	int32 CountExposedProperties(const TSharedPtr<FJsonValue>& Value) const;
	FString InlineValue(const TSharedPtr<FJsonValue>& Value) const;
	FString Padding(int32 Level) const;

	FUeAgentStructuredJsonFormatOptions Options;
};

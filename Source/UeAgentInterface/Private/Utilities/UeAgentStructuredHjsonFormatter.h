#pragma once

#include "CoreMinimal.h"
#include "HAL/PlatformTime.h"

class FJsonObject;
class FJsonValue;

struct UEAGENTINTERFACE_API FUeAgentHjsonSourceLocation
{
	FString FilePath;
	int32 Line = 0;
	int32 Column = 0;
};

struct UEAGENTINTERFACE_API FUeAgentHjsonDiagnostic
{
	FString DialectId = TEXT("uai-hjson-authoring/1");
	FString Phase;
	FString Code;
	FString SourceFile;
	FString JsonPointer = TEXT("/");
	int32 Line = 0;
	int32 Column = 0;
	FString Field;
	FString Message;

	FString ToString() const;
};

// Compatibility name retained for callers that model parse failures explicitly.
using FUeAgentHjsonParseError = FString;

struct UEAGENTINTERFACE_API FUeAgentStructuredHjsonDocument
{
	TSharedPtr<FJsonObject> Root;
	FString SourceFile;
	FString SourceText;
	TMap<FString, FUeAgentHjsonSourceLocation> SourceLocations;

	const FUeAgentHjsonSourceLocation* FindSourceLocation(const FString& JsonPointer) const;
};

struct UEAGENTINTERFACE_API FUeAgentStructuredResourceBudget
{
	int64 MaxInputBytes = 128ll * 1024ll * 1024ll;
	int64 MaxNodes = 1000000ll;
	int64 MaxItems = 2000000ll;
	int64 MaxWorkUnits = 64ll * 1024ll * 1024ll;
	int64 MaxOutputChars = 64ll * 1024ll * 1024ll;
	double MaxElapsedSeconds = 30.0;
	int64 MaxEstimatedMemoryBytes = 512ll * 1024ll * 1024ll;
};

struct UEAGENTINTERFACE_API FUeAgentStructuredResourceUsage
{
	int64 InputBytes = 0;
	int64 Nodes = 0;
	int64 Items = 0;
	int64 WorkUnits = 0;
	int64 OutputChars = 0;
	double ElapsedSeconds = 0.0;
	int64 EstimatedMemoryBytes = 0;
};

class UEAGENTINTERFACE_API FUeAgentStructuredResourceTracker
{
public:
	using FClock = TFunction<double()>;

	explicit FUeAgentStructuredResourceTracker(
		const FUeAgentStructuredResourceBudget& InBudget =
			FUeAgentStructuredResourceBudget(),
		FClock InClock = FClock())
		: Budget(InBudget)
		, Clock(MoveTemp(InClock))
	{
		if (!Clock)
		{
			Clock = []() { return FPlatformTime::Seconds(); };
		}
		StartSeconds = Clock();
	}

	bool ChargeInputBytes(
		const int64 Amount,
		const FString& JsonPointer,
		FString& OutError)
	{
		return ChargeInteger(
			TEXT("input_bytes"),
			Amount,
			Budget.MaxInputBytes,
			Usage.InputBytes,
			JsonPointer,
			OutError);
	}

	bool ChargeNodes(
		const int64 Amount,
		const FString& JsonPointer,
		FString& OutError)
	{
		return ChargeInteger(
			TEXT("nodes"),
			Amount,
			Budget.MaxNodes,
			Usage.Nodes,
			JsonPointer,
			OutError);
	}

	bool ChargeItems(
		const int64 Amount,
		const FString& JsonPointer,
		FString& OutError)
	{
		return ChargeInteger(
			TEXT("items"),
			Amount,
			Budget.MaxItems,
			Usage.Items,
			JsonPointer,
			OutError);
	}

	bool ChargeWorkUnits(
		const int64 Amount,
		const FString& JsonPointer,
		FString& OutError)
	{
		const int64 PreviousWorkUnits = Usage.WorkUnits;
		if (!ChargeInteger(
				TEXT("work_units"),
				Amount,
				Budget.MaxWorkUnits,
				Usage.WorkUnits,
				JsonPointer,
				OutError))
		{
			return false;
		}
		return PreviousWorkUnits / 1024ll == Usage.WorkUnits / 1024ll
			|| Checkpoint(JsonPointer, OutError);
	}

	bool ChargeOutputChars(
		const int64 Amount,
		const FString& JsonPointer,
		FString& OutError)
	{
		return ChargeInteger(
			TEXT("output_chars"),
			Amount,
			Budget.MaxOutputChars,
			Usage.OutputChars,
			JsonPointer,
			OutError);
	}

	bool ChargeEstimatedMemoryBytes(
		const int64 Amount,
		const FString& JsonPointer,
		FString& OutError)
	{
		return ChargeInteger(
			TEXT("estimated_memory_bytes"),
			Amount,
			Budget.MaxEstimatedMemoryBytes,
			Usage.EstimatedMemoryBytes,
			JsonPointer,
			OutError);
	}

	bool Checkpoint(const FString& JsonPointer, FString& OutError)
	{
		if (!OutError.IsEmpty())
		{
			return false;
		}
		Usage.ElapsedSeconds = FMath::Max(0.0, Clock() - StartSeconds);
		if (Budget.MaxElapsedSeconds >= 0.0
			&& Usage.ElapsedSeconds > Budget.MaxElapsedSeconds)
		{
			const FString Pointer = NormalizePointer(JsonPointer);
			OutError = FString::Printf(
				TEXT("resource_budget_exceeded:code=resource_budget_exceeded ")
				TEXT("kind=elapsed_seconds resource=elapsed_seconds ")
				TEXT("limit=%.17g consumed=%.17g path=%s pointer=%s"),
				Budget.MaxElapsedSeconds,
				Usage.ElapsedSeconds,
				*Pointer,
				*Pointer);
			return false;
		}
		OutError.Reset();
		return true;
	}

	const FUeAgentStructuredResourceBudget& GetBudget() const
	{
		return Budget;
	}

	const FUeAgentStructuredResourceUsage& GetUsage() const
	{
		return Usage;
	}

private:
	static FString NormalizePointer(const FString& JsonPointer)
	{
		if (JsonPointer.IsEmpty())
		{
			return TEXT("/");
		}
		return JsonPointer.StartsWith(TEXT("/"))
			? JsonPointer
			: TEXT("/") + JsonPointer;
	}

	static bool ChargeInteger(
		const TCHAR* Resource,
		const int64 Amount,
		const int64 Limit,
		int64& InOutConsumed,
		const FString& JsonPointer,
		FString& OutError)
	{
		if (!OutError.IsEmpty())
		{
			return false;
		}
		if (Amount < 0)
		{
			const FString Pointer = NormalizePointer(JsonPointer);
			OutError = FString::Printf(
				TEXT("resource_budget_invalid_charge:code=resource_budget_invalid_charge ")
					TEXT("kind=%s resource=%s limit=%lld consumed=%lld ")
					TEXT("path=%s pointer=%s"),
				Resource,
				Resource,
				static_cast<long long>(Limit),
				static_cast<long long>(Amount),
				*Pointer,
				*Pointer);
			return false;
		}
		if (Amount > MAX_int64 - InOutConsumed)
		{
			InOutConsumed = MAX_int64;
		}
		else
		{
			InOutConsumed += Amount;
		}
		if (Limit >= 0 && InOutConsumed > Limit)
		{
			const FString Pointer = NormalizePointer(JsonPointer);
			OutError = FString::Printf(
				TEXT("resource_budget_exceeded:code=resource_budget_exceeded ")
					TEXT("kind=%s resource=%s limit=%lld consumed=%lld ")
					TEXT("path=%s pointer=%s"),
				Resource,
				Resource,
				static_cast<long long>(Limit),
				static_cast<long long>(InOutConsumed),
				*Pointer,
				*Pointer);
			return false;
		}
		OutError.Reset();
		return true;
	}

	FUeAgentStructuredResourceBudget Budget;
	FUeAgentStructuredResourceUsage Usage;
	FClock Clock;
	double StartSeconds = 0.0;
};

struct UEAGENTINTERFACE_API FUeAgentStructuredHjsonWriteOptions
{
	TMap<FString, TArray<FString>> PreferredKeyOrderByJsonPointer;
	FUeAgentStructuredResourceBudget ResourceBudget;
};

class UEAGENTINTERFACE_API FUeAgentStructuredHjsonFormatter
{
public:
	static const TCHAR* DialectId();

	static FString WriteObject(const TSharedRef<FJsonObject>& Object);
	static FString WriteObject(
		const TSharedRef<FJsonObject>& Object,
		const FUeAgentStructuredHjsonWriteOptions& Options);

	static FString WriteValue(const TSharedRef<FJsonValue>& Value);
	static FString WriteValue(
		const TSharedRef<FJsonValue>& Value,
		const FUeAgentStructuredHjsonWriteOptions& Options);

	static bool TryWriteObject(
		const TSharedRef<FJsonObject>& Object,
		FString& OutText,
		FString& OutError);

	static bool TryWriteObject(
		const TSharedRef<FJsonObject>& Object,
		const FUeAgentStructuredHjsonWriteOptions& Options,
		FString& OutText,
		FString& OutError);

	static bool TryWriteObject(
		const TSharedRef<FJsonObject>& Object,
		const FUeAgentStructuredHjsonWriteOptions& Options,
		FUeAgentStructuredResourceTracker& ResourceTracker,
		FString& OutText,
		FString& OutError);

	static bool TryWriteValue(
		const TSharedRef<FJsonValue>& Value,
		FString& OutText,
		FString& OutError);

	static bool TryWriteValue(
		const TSharedRef<FJsonValue>& Value,
		const FUeAgentStructuredHjsonWriteOptions& Options,
		FString& OutText,
		FString& OutError);

	static bool TryWriteValue(
		const TSharedRef<FJsonValue>& Value,
		const FUeAgentStructuredHjsonWriteOptions& Options,
		FUeAgentStructuredResourceTracker& ResourceTracker,
		FString& OutText,
		FString& OutError);

	static bool IsBareKey(const FString& Key);

	static bool IsBareString(const FString& Value);

	static FString EscapeQuotedString(const FString& Value);
};

class UEAGENTINTERFACE_API FUeAgentStructuredHjsonParser
{
public:
	static bool ParseDocument(
		const FString& Text,
		const FString& FilePath,
		FUeAgentStructuredHjsonDocument& OutDocument,
		FString& OutError);

	static bool ParseDocument(
		const FString& Text,
		const FString& FilePath,
		const FUeAgentStructuredResourceBudget& ResourceBudget,
		FUeAgentStructuredHjsonDocument& OutDocument,
		FString& OutError);

	static bool ParseDocument(
		const FString& Text,
		const FString& FilePath,
		FUeAgentStructuredResourceTracker& ResourceTracker,
		bool bInputPrecharged,
		FUeAgentStructuredHjsonDocument& OutDocument,
		FString& OutError);

	static bool ParseObject(
		const FString& Text,
		const FString& FilePath,
		TSharedPtr<FJsonObject>& OutObject,
		FString& OutError);

	static bool ParseObject(
		const FString& Text,
		const FString& FilePath,
		const FUeAgentStructuredResourceBudget& ResourceBudget,
		TSharedPtr<FJsonObject>& OutObject,
		FString& OutError);
};

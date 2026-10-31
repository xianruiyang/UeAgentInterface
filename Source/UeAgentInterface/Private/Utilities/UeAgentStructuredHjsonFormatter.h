#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "HAL/PlatformTime.h"

class FJsonObject;
class FJsonValue;
namespace UE::UAI::StateSemantic { class FUeAgentCanonicalValue; }

struct UEAGENTINTERFACE_API FUeAgentHjsonSourceLocation
{
	FString FilePath;
	int32 Line = 0;
	int32 Column = 0;
};

struct UEAGENTINTERFACE_API FUeAgentStructuredHjsonDocument
{
	TSharedPtr<FJsonObject> Root;
	FString SourceFile;
	FString SourceText;
	TMap<FString, FUeAgentHjsonSourceLocation> SourceLocations;

	const FUeAgentHjsonSourceLocation* FindSourceLocation(const FString& JsonPointer) const;
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

	explicit FUeAgentStructuredResourceTracker(FClock InClock = FClock())
		: Clock(MoveTemp(InClock))
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		return ChargeInteger(
			TEXT("input_bytes"),
			Amount,
			Usage.InputBytes,
			JsonPointer,
			OutError);
	}

	bool ChargeNodes(
		const int64 Amount,
		const FString& JsonPointer,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		return ChargeInteger(
			TEXT("nodes"),
			Amount,
			Usage.Nodes,
			JsonPointer,
			OutError);
	}

	bool ChargeItems(
		const int64 Amount,
		const FString& JsonPointer,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		return ChargeInteger(
			TEXT("items"),
			Amount,
			Usage.Items,
			JsonPointer,
			OutError);
	}

	bool ChargeWorkUnits(
		const int64 Amount,
		const FString& JsonPointer,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		const int64 PreviousWorkUnits = Usage.WorkUnits;
		if (!ChargeInteger(
				TEXT("work_units"),
				Amount,
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		return ChargeInteger(
			TEXT("output_chars"),
			Amount,
			Usage.OutputChars,
			JsonPointer,
			OutError);
	}

	bool ChargeEstimatedMemoryBytes(
		const int64 Amount,
		const FString& JsonPointer,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		return ChargeInteger(
			TEXT("estimated_memory_bytes"),
			Amount,
			Usage.EstimatedMemoryBytes,
			JsonPointer,
			OutError);
	}

	bool Checkpoint(const FString& JsonPointer, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!OutError.IsEmpty())
		{
			return false;
		}
		Usage.ElapsedSeconds = FMath::Max(0.0, Clock() - StartSeconds);
		OutError.Reset();
		return true;
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
		int64& InOutConsumed,
		const FString& JsonPointer,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!OutError.IsEmpty())
		{
			return false;
		}
		if (Amount < 0)
		{
			const FString Pointer = NormalizePointer(JsonPointer);
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("resource_charge_invalid_amount"),
				TEXT("Resource accounting received a negative charge."), TEXT("resource_accounting"));
			OutError.ResourceDimension = Resource;
			OutError.ResourceObserved = Amount;
			OutError.Source.JsonPath = Pointer;
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
		OutError.Reset();
		return true;
	}

	FUeAgentStructuredResourceUsage Usage;
	FClock Clock;
	double StartSeconds = 0.0;
};

struct UEAGENTINTERFACE_API FUeAgentStructuredHjsonWriteOptions
{
	using FStableLeafProjection = bool (*)(const UE::UAI::StateSemantic::FUeAgentCanonicalValue&,
		TFunctionRef<bool(int64)>, TSharedPtr<FJsonValue>&, UE::UAI::Contract::FDiagnostic&);
	TMap<FString, TArray<FString>> PreferredKeyOrderByJsonPointer;
	/** Optional semantic leaf representation; unset means literal HJSON kinds. */
	TFunction<bool(const UE::UAI::StateSemantic::FUeAgentCanonicalValue&,
		TFunctionRef<bool(int64)>, TSharedPtr<FJsonValue>&, UE::UAI::Contract::FDiagnostic&)> CanonicalLeafProjection;
	/** Stateless projection owner. Dynamic callbacks above keep the uncached path. */
	FStableLeafProjection StableCanonicalLeafProjection = nullptr;
};

class UEAGENTINTERFACE_API FUeAgentStructuredHjsonFormatter
{
public:
	static const TCHAR* DialectId();
	static bool TryWriteValue(
		const UE::UAI::StateSemantic::FUeAgentCanonicalValue& Value,
		FString& OutText,
		UE::UAI::Contract::FDiagnostic& OutError);
	static bool TryWriteValue(
		const UE::UAI::StateSemantic::FUeAgentCanonicalValue& Value,
		const FUeAgentStructuredHjsonWriteOptions& Options,
		FString& OutText,
		UE::UAI::Contract::FDiagnostic& OutError);

	static bool TryWriteObject(
		const UE::UAI::StateSemantic::FUeAgentCanonicalValue& Object,
		FString& OutText,
		UE::UAI::Contract::FDiagnostic& OutError);
	static bool TryWriteObject(
		const UE::UAI::StateSemantic::FUeAgentCanonicalValue& Object,
		const FUeAgentStructuredHjsonWriteOptions& Options,
		FUeAgentStructuredResourceTracker& ResourceTracker,
		FString& OutText,
		UE::UAI::Contract::FDiagnostic& OutError);

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
		UE::UAI::Contract::FDiagnostic& OutError);

	static bool TryWriteObject(
		const TSharedRef<FJsonObject>& Object,
		const FUeAgentStructuredHjsonWriteOptions& Options,
		FString& OutText,
		UE::UAI::Contract::FDiagnostic& OutError);

	static bool TryWriteObject(
		const TSharedRef<FJsonObject>& Object,
		const FUeAgentStructuredHjsonWriteOptions& Options,
		FUeAgentStructuredResourceTracker& ResourceTracker,
		FString& OutText,
		UE::UAI::Contract::FDiagnostic& OutError);

	static bool TryWriteValue(
		const TSharedRef<FJsonValue>& Value,
		FString& OutText,
		UE::UAI::Contract::FDiagnostic& OutError);

	static bool TryWriteValue(
		const TSharedRef<FJsonValue>& Value,
		const FUeAgentStructuredHjsonWriteOptions& Options,
		FString& OutText,
		UE::UAI::Contract::FDiagnostic& OutError);

	static bool TryWriteValue(
		const TSharedRef<FJsonValue>& Value,
		const FUeAgentStructuredHjsonWriteOptions& Options,
		FUeAgentStructuredResourceTracker& ResourceTracker,
		FString& OutText,
		UE::UAI::Contract::FDiagnostic& OutError);

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
		UE::UAI::Contract::FDiagnostic& OutError);

	static bool ParseDocument(
		const FString& Text,
		const FString& FilePath,
		FUeAgentStructuredResourceTracker& ResourceTracker,
		bool bInputPrecharged,
		FUeAgentStructuredHjsonDocument& OutDocument,
		UE::UAI::Contract::FDiagnostic& OutError);

	static bool ParseObject(
		const FString& Text,
		const FString& FilePath,
		TSharedPtr<FJsonObject>& OutObject,
		UE::UAI::Contract::FDiagnostic& OutError);

};

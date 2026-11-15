#include "Core/ContextResource/UeAgentContextResource.h"

#include "Containers/StringConv.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/ScopeLock.h"

namespace UE::UAI::ContextResource
{
	namespace
	{
		struct FDimensionName
		{
			EUeAgentResourceDimension Dimension;
			const TCHAR* Name;
		};

		const FDimensionName DimensionNames[] = {
			{EUeAgentResourceDimension::InputBytes, TEXT("input_bytes")},
			{EUeAgentResourceDimension::OutputBytes, TEXT("output_bytes")},
			{EUeAgentResourceDimension::StringBytes, TEXT("string_bytes")},
			{EUeAgentResourceDimension::Nodes, TEXT("nodes")},
			{EUeAgentResourceDimension::Items, TEXT("items")},
			{EUeAgentResourceDimension::Files, TEXT("files")},
			{EUeAgentResourceDimension::ReferenceEdges, TEXT("reference_edges")},
			{EUeAgentResourceDimension::WorkUnits, TEXT("work_units")},
			{EUeAgentResourceDimension::EstimatedMemoryBytes, TEXT("estimated_memory_bytes")},
		};

		bool IsHighSurrogate(const uint32 CodeUnit)
		{
			return CodeUnit >= 0xD800 && CodeUnit <= 0xDBFF;
		}

		bool IsLowSurrogate(const uint32 CodeUnit)
		{
			return CodeUnit >= 0xDC00 && CodeUnit <= 0xDFFF;
		}
	}

	FString LexToString(const EUeAgentResourceDimension Dimension)
	{
		for (const FDimensionName& Entry : DimensionNames)
		{
			if (Entry.Dimension == Dimension)
			{
				return Entry.Name;
			}
		}
		return TEXT("unknown");
	}

	bool TryParseResourceDimension(
		const FString& Value,
		EUeAgentResourceDimension& OutDimension)
	{
		for (const FDimensionName& Entry : DimensionNames)
		{
			if (Value.Equals(Entry.Name, ESearchCase::CaseSensitive))
			{
				OutDimension = Entry.Dimension;
				return true;
			}
		}
		return false;
	}

	FUeAgentResourceUsage::FUeAgentResourceUsage(
		const TMap<EUeAgentResourceDimension, int64>& InValues)
		: Values(InValues)
	{
	}

	int64 FUeAgentResourceUsage::Consumed(
		const EUeAgentResourceDimension Dimension) const
	{
		const int64* Value = Values.Find(Dimension);
		return Value ? *Value : 0;
	}

	FUeAgentResourceScope::FUeAgentResourceScope(
		FUeAgentResourceTracker& InTracker,
		FString InPath)
		: Tracker(&InTracker)
		, Path(MoveTemp(InPath))
	{
		checkf(!Path.TrimStartAndEnd().IsEmpty(), TEXT("Resource scope path must be non-empty."));
	}

	bool FUeAgentResourceScope::TryCharge(
		const EUeAgentResourceDimension Dimension,
		const int64 Amount,
		FUeAgentResourceChargeError& OutError)
	{
		return Tracker->TryCharge(Dimension, Amount, Path, OutError);
	}

	bool FUeAgentResourceScope::TryChargeMany(
		const TMap<EUeAgentResourceDimension, int64>& Amounts,
		FUeAgentResourceChargeError& OutError)
	{
		return Tracker->TryChargeMany(Amounts, Path, OutError);
	}

	bool FUeAgentResourceScope::TryChargeMany(
		std::initializer_list<TPair<EUeAgentResourceDimension, int64>> Amounts,
		FUeAgentResourceChargeError& OutError)
	{
		return Tracker->TryChargeMany(Amounts, Path, OutError);
	}

	const FString& FUeAgentResourceScope::GetPath() const
	{
		return Path;
	}

	FUeAgentResourceTracker::FUeAgentResourceTracker() = default;
	FUeAgentResourceTracker::FUeAgentResourceTracker(bool bInCollectUsage) : bCollectUsage(bInCollectUsage) {}

	FUeAgentResourceScope FUeAgentResourceTracker::Scope(FString Path)
	{
		return FUeAgentResourceScope(*this, MoveTemp(Path));
	}

	bool FUeAgentResourceTracker::TryCharge(
		const EUeAgentResourceDimension Dimension,
		const int64 Amount,
		const FString& Path,
		FUeAgentResourceChargeError& OutError)
	{
		if (!bCollectUsage) return true;
		if (Amount < 0)
		{
			OutError = FUeAgentResourceChargeError::Failure(TEXT("resource_amount_invalid"), TEXT("Resource accounting rejected a negative amount."), TEXT("resource_accounting"));
			OutError.ResourceDimension = LexToString(Dimension);
			OutError.ResourceObserved = Amount;
			OutError.Source.JsonPath = Path;
			OutError.StoppedEarly = true;
			return false;
		}
		FScopeLock Lock(&Mutex);
		int64& Current = Values.FindOrAdd(Dimension);
		Current += FMath::Min(Amount, MAX_int64 - Current);
		return true;
	}

	bool FUeAgentResourceTracker::TryChargeMany(
		const TMap<EUeAgentResourceDimension, int64>& Amounts,
		const FString& Path,
		FUeAgentResourceChargeError& OutError)
	{
		if (!bCollectUsage) return true;
		// Validate the complete immutable request before touching usage. Once
		// admitted, the same lock protects every dimension's saturating update;
		// no per-token/per-node temporary Candidates map is needed.
		for (const TPair<EUeAgentResourceDimension, int64>& Pair : Amounts)
		{
			if (Pair.Value < 0)
			{
				OutError = FUeAgentResourceChargeError::Failure(TEXT("resource_amount_invalid"), TEXT("Resource accounting rejected a negative amount."), TEXT("resource_accounting"));
				OutError.ResourceDimension = LexToString(Pair.Key);
				OutError.ResourceObserved = Pair.Value;
				OutError.Source.JsonPath = Path;
				OutError.StoppedEarly = true;
				return false;
			}
		}
		FScopeLock Lock(&Mutex);
		for (const TPair<EUeAgentResourceDimension, int64>& Pair : Amounts)
		{
			int64& Current = Values.FindOrAdd(Pair.Key);
			Current += FMath::Min(Pair.Value, MAX_int64 - Current);
		}
		return true;
	}

	bool FUeAgentResourceTracker::TryChargeMany(
		std::initializer_list<TPair<EUeAgentResourceDimension, int64>> Amounts,
		const FString& Path, FUeAgentResourceChargeError& OutError)
	{
		if (!bCollectUsage) return true;
		// The dimension list is bounded by its caller, not an input-size quota.
		// Preserve the map overload's all-or-nothing validation and duplicate
		// semantics without allocating a map for every scalar or token.
		const auto Visit = [&](auto&& Consumer) {
			for (auto It = Amounts.begin(); It != Amounts.end(); ++It) {
				bool bFirst = true;
				for (auto Prior = Amounts.begin(); Prior != It; ++Prior)
					if (Prior->Key == It->Key) { bFirst = false; break; }
				if (!bFirst) continue;
				const auto* Last = It;
				for (auto Next = It + 1; Next != Amounts.end(); ++Next)
					if (Next->Key == It->Key) Last = Next;
				if (!Consumer(It->Key, Last->Value)) return false;
			}
			return true;
		};
		if (!Visit([&](EUeAgentResourceDimension Dimension, int64 Amount) {
			if (Amount >= 0) return true;
			OutError = FUeAgentResourceChargeError::Failure(TEXT("resource_amount_invalid"), TEXT("Resource accounting rejected a negative amount."), TEXT("resource_accounting"));
			OutError.ResourceDimension = LexToString(Dimension);
			OutError.ResourceObserved = Amount;
			OutError.Source.JsonPath = Path;
			OutError.StoppedEarly = true;
			return false;
		})) return false;
		FScopeLock Lock(&Mutex);
		return Visit([&](EUeAgentResourceDimension Dimension, int64 Amount) {
			int64& Current = Values.FindOrAdd(Dimension);
			Current += FMath::Min(Amount, MAX_int64 - Current);
			return true;
		});
	}

	FUeAgentResourceUsage FUeAgentResourceTracker::Snapshot() const
	{
		if (!bCollectUsage) return FUeAgentResourceUsage({});
		FScopeLock Lock(&Mutex);
		return FUeAgentResourceUsage(Values);
	}

	FUeAgentTextUnits MeasureText(const FString& Text)
	{
		const FTCHARToUTF8 Utf8(*Text, Text.Len());
		int64 ScalarCount = 0;
		for (int32 Index = 0; Index < Text.Len(); ++Index)
		{
			const uint32 Current = static_cast<uint32>(Text[Index]);
			if (IsHighSurrogate(Current)
				&& Index + 1 < Text.Len()
				&& IsLowSurrogate(static_cast<uint32>(Text[Index + 1])))
			{
				++Index;
			}
			++ScalarCount;
		}

		FUeAgentTextUnits Result;
		Result.Utf8Bytes = Utf8.Length();
		Result.Utf16CodeUnits = Text.Len();
		Result.UnicodeScalars = ScalarCount;
		return Result;
	}

	FUeAgentOperationDeadline::FUeAgentOperationDeadline()
		: StartedAt(0.0)
		, DeadlineAt(0.0)
		, Clock([]() { return 0.0; })
	{
	}

	FUeAgentOperationDeadline FUeAgentOperationDeadline::FromTimeout(
		const double TimeoutSeconds,
		FUeAgentMonotonicClock InClock)
	{
		checkf(InClock, TEXT("Operation deadline clock must be provided."));
		checkf(
			FMath::IsFinite(TimeoutSeconds) && TimeoutSeconds >= 0.0,
			TEXT("Operation timeout must be finite and non-negative."));
		const double StartedAt = InClock();
		checkf(FMath::IsFinite(StartedAt), TEXT("Operation clock must return a finite value."));
		return FUeAgentOperationDeadline(
			StartedAt,
			StartedAt + TimeoutSeconds,
			MoveTemp(InClock));
	}

	FUeAgentOperationDeadline::FUeAgentOperationDeadline(
		const double InStartedAt,
		const double InDeadlineAt,
		FUeAgentMonotonicClock InClock)
		: StartedAt(InStartedAt)
		, DeadlineAt(InDeadlineAt)
		, Clock(MoveTemp(InClock))
	{
	}

	double FUeAgentOperationDeadline::GetStartedAt() const
	{
		return StartedAt;
	}

	double FUeAgentOperationDeadline::GetDeadlineAt() const
	{
		return DeadlineAt;
	}

	double FUeAgentOperationDeadline::RemainingSeconds() const
	{
		return FMath::Max(0.0, DeadlineAt - Clock());
	}

	bool FUeAgentOperationDeadline::CanContinue() const
	{
		return true;
	}

	bool FUeAgentOperationDeadline::TryCheckpoint(
		const FString& Stage,
		FUeAgentOperationDeadlineExceeded& OutError) const
	{
		return true;
	}

	class FUeAgentCancellationState
	{
	public:
		const FUeAgentCancellationRequest& Request(
			const FString& Reason,
			const double RequestedAt)
		{
			FScopeLock Lock(&Mutex);
			if (!RequestValue.IsSet())
			{
				checkf(
					!Reason.TrimStartAndEnd().IsEmpty(),
					TEXT("Cancellation reason must be non-empty."));
				checkf(
					FMath::IsFinite(RequestedAt),
					TEXT("Cancellation time must be finite."));
				RequestValue.Emplace(FUeAgentCancellationRequest{Reason, RequestedAt});
			}
			return RequestValue.GetValue();
		}

		bool IsCancelled() const
		{
			FScopeLock Lock(&Mutex);
			return RequestValue.IsSet();
		}

		const FUeAgentCancellationRequest* GetRequest() const
		{
			FScopeLock Lock(&Mutex);
			return RequestValue.IsSet() ? &RequestValue.GetValue() : nullptr;
		}

	private:
		mutable FCriticalSection Mutex;
		TOptional<FUeAgentCancellationRequest> RequestValue;
	};

	FUeAgentCancellationToken::FUeAgentCancellationToken(
		TSharedRef<FUeAgentCancellationState, ESPMode::ThreadSafe> InState)
		: State(MoveTemp(InState))
	{
	}

	bool FUeAgentCancellationToken::IsCancelled() const
	{
		return State->IsCancelled();
	}

	const FUeAgentCancellationRequest*
	FUeAgentCancellationToken::GetRequest() const
	{
		return State->GetRequest();
	}

	bool FUeAgentCancellationToken::TryCheckpoint(
		FUeAgentCancellationRequested& OutError) const
	{
		const FUeAgentCancellationRequest* Request = State->GetRequest();
		if (!Request)
		{
			return true;
		}
		OutError = Contract::FDiagnostic::Failure(TEXT("cancellation_requested"),
			Request->Reason.IsEmpty() ? TEXT("Cancellation was requested.") : Request->Reason, TEXT("cancellation"));
		OutError.Schema = TEXT("uai.runtime.diagnostic/1");
		OutError.CancellationRequestedAt = Request->RequestedAt;
		return false;
	}

	FUeAgentCancellationSource::FUeAgentCancellationSource(
		FUeAgentMonotonicClock InClock)
		: State(MakeShared<FUeAgentCancellationState, ESPMode::ThreadSafe>())
		, Clock(MoveTemp(InClock))
	{
		checkf(Clock, TEXT("Cancellation clock must be provided."));
	}

	FUeAgentCancellationToken FUeAgentCancellationSource::GetToken() const
	{
		return FUeAgentCancellationToken(State);
	}

	const FUeAgentCancellationRequest& FUeAgentCancellationSource::Request(
		const FString& Reason)
	{
		return State->Request(Reason, Clock());
	}
}

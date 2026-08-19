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

	FUeAgentResourceBudget::FUeAgentResourceBudget(
		const TMap<EUeAgentResourceDimension, int64>& InLimits)
		: Limits(InLimits)
	{
		for (const TPair<EUeAgentResourceDimension, int64>& Pair : Limits)
		{
			checkf(Pair.Value >= 0, TEXT("Resource limits must be non-negative."));
		}
	}

	TOptional<int64> FUeAgentResourceBudget::LimitFor(
		const EUeAgentResourceDimension Dimension) const
	{
		const int64* Limit = Limits.Find(Dimension);
		return Limit ? TOptional<int64>(*Limit) : TOptional<int64>();
	}

	const TMap<EUeAgentResourceDimension, int64>&
	FUeAgentResourceBudget::GetLimits() const
	{
		return Limits;
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
		FUeAgentResourceBudgetExceeded& OutError)
	{
		return Tracker->TryCharge(Dimension, Amount, Path, OutError);
	}

	bool FUeAgentResourceScope::TryChargeMany(
		const TMap<EUeAgentResourceDimension, int64>& Amounts,
		FUeAgentResourceBudgetExceeded& OutError)
	{
		return Tracker->TryChargeMany(Amounts, Path, OutError);
	}

	const FString& FUeAgentResourceScope::GetPath() const
	{
		return Path;
	}

	FUeAgentResourceTracker::FUeAgentResourceTracker(
		FUeAgentResourceBudget InBudget)
		: Budget(MoveTemp(InBudget))
	{
	}

	FUeAgentResourceScope FUeAgentResourceTracker::Scope(FString Path)
	{
		return FUeAgentResourceScope(*this, MoveTemp(Path));
	}

	bool FUeAgentResourceTracker::TryCharge(
		const EUeAgentResourceDimension Dimension,
		const int64 Amount,
		const FString& Path,
		FUeAgentResourceBudgetExceeded& OutError)
	{
		return TryChargeMany({{Dimension, Amount}}, Path, OutError);
	}

	bool FUeAgentResourceTracker::TryChargeMany(
		const TMap<EUeAgentResourceDimension, int64>& Amounts,
		const FString& Path,
		FUeAgentResourceBudgetExceeded& OutError)
	{
		FScopeLock Lock(&Mutex);
		TMap<EUeAgentResourceDimension, int64> Candidates;
		for (const TPair<EUeAgentResourceDimension, int64>& Pair : Amounts)
		{
			if (Pair.Value < 0)
			{
				OutError.Code = TEXT("resource_amount_invalid");
				OutError.Dimension = Pair.Key;
				OutError.Limit = 0;
				OutError.Observed = Pair.Value;
				OutError.Path = Path;
				OutError.bStoppedEarly = true;
				return false;
			}
			Candidates.Add(Pair.Key, Values.FindRef(Pair.Key) + Pair.Value);
		}

		for (const FDimensionName& Entry : DimensionNames)
		{
			const int64* Observed = Candidates.Find(Entry.Dimension);
			const TOptional<int64> Limit = Budget.LimitFor(Entry.Dimension);
			if (Observed && Limit.IsSet() && *Observed > Limit.GetValue())
			{
				OutError.Code = TEXT("resource_budget_exceeded");
				OutError.Dimension = Entry.Dimension;
				OutError.Limit = Limit.GetValue();
				OutError.Observed = *Observed;
				OutError.Path = Path;
				OutError.bStoppedEarly = true;
				return false;
			}
		}

		for (const TPair<EUeAgentResourceDimension, int64>& Pair : Candidates)
		{
			Values.Add(Pair.Key, Pair.Value);
		}
		return true;
	}

	FUeAgentResourceUsage FUeAgentResourceTracker::Snapshot() const
	{
		FScopeLock Lock(&Mutex);
		return FUeAgentResourceUsage(Values);
	}

	const FUeAgentResourceBudget& FUeAgentResourceTracker::GetBudget() const
	{
		return Budget;
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
		return Clock() < DeadlineAt;
	}

	bool FUeAgentOperationDeadline::TryCheckpoint(
		const FString& Stage,
		FUeAgentOperationDeadlineExceeded& OutError) const
	{
		checkf(!Stage.TrimStartAndEnd().IsEmpty(), TEXT("Deadline stage must be non-empty."));
		const double ObservedAt = Clock();
		if (ObservedAt < DeadlineAt)
		{
			return true;
		}
		OutError.Code = TEXT("operation_deadline_exceeded");
		OutError.Stage = Stage;
		OutError.DeadlineAt = DeadlineAt;
		OutError.ObservedAt = ObservedAt;
		return false;
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
		OutError.Code = TEXT("cancellation_requested");
		OutError.Reason = Request->Reason;
		OutError.RequestedAt = Request->RequestedAt;
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

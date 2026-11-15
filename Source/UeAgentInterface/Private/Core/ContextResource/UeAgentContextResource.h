#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "HAL/CriticalSection.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"
#include <initializer_list>

namespace UE::UAI::ContextResource
{
	enum class EUeAgentResourceDimension : uint8
	{
		InputBytes,
		OutputBytes,
		StringBytes,
		Nodes,
		Items,
		Files,
		ReferenceEdges,
		WorkUnits,
		EstimatedMemoryBytes,
	};

	UEAGENTINTERFACE_API FString LexToString(EUeAgentResourceDimension Dimension);
	UEAGENTINTERFACE_API bool TryParseResourceDimension(
		const FString& Value,
		EUeAgentResourceDimension& OutDimension);

	using FUeAgentResourceChargeError = UE::UAI::Contract::FDiagnostic;

	class UEAGENTINTERFACE_API FUeAgentResourceUsage
	{
	public:
		int64 Consumed(EUeAgentResourceDimension Dimension) const;

	private:
		explicit FUeAgentResourceUsage(
			const TMap<EUeAgentResourceDimension, int64>& InValues);

		TMap<EUeAgentResourceDimension, int64> Values;

		friend class FUeAgentResourceTracker;
	};

	class FUeAgentResourceTracker;

	class UEAGENTINTERFACE_API FUeAgentResourceScope
	{
	public:
		FUeAgentResourceScope(FUeAgentResourceTracker& InTracker, FString InPath);

		bool TryCharge(
			EUeAgentResourceDimension Dimension,
			int64 Amount,
			FUeAgentResourceChargeError& OutError);
		bool TryChargeMany(
			const TMap<EUeAgentResourceDimension, int64>& Amounts,
			FUeAgentResourceChargeError& OutError);
		bool TryChargeMany(
			std::initializer_list<TPair<EUeAgentResourceDimension, int64>> Amounts,
			FUeAgentResourceChargeError& OutError);
		const FString& GetPath() const;

	private:
		FUeAgentResourceTracker* Tracker;
		FString Path;
	};

	class UEAGENTINTERFACE_API FUeAgentResourceTracker
	{
	public:
		FUeAgentResourceTracker();
		explicit FUeAgentResourceTracker(bool bInCollectUsage);
		bool CollectsUsage() const { return bCollectUsage; }
		FUeAgentResourceTracker(const FUeAgentResourceTracker&) = delete;
		FUeAgentResourceTracker& operator=(const FUeAgentResourceTracker&) = delete;

		FUeAgentResourceScope Scope(FString Path);
		bool TryCharge(
			EUeAgentResourceDimension Dimension,
			int64 Amount,
			const FString& Path,
			FUeAgentResourceChargeError& OutError);
		bool TryChargeMany(
			const TMap<EUeAgentResourceDimension, int64>& Amounts,
			const FString& Path,
			FUeAgentResourceChargeError& OutError);
		/** Fixed requests do not need a temporary heap map. Duplicate dimensions
		 * retain TMap initializer semantics: first position, last supplied value. */
		bool TryChargeMany(
			std::initializer_list<TPair<EUeAgentResourceDimension, int64>> Amounts,
			const FString& Path,
			FUeAgentResourceChargeError& OutError);
		FUeAgentResourceUsage Snapshot() const;
	private:
		const bool bCollectUsage = true;
		TMap<EUeAgentResourceDimension, int64> Values;
		mutable FCriticalSection Mutex;
	};

	struct FUeAgentTextUnits
	{
		int64 Utf8Bytes = 0;
		int64 Utf16CodeUnits = 0;
		int64 UnicodeScalars = 0;
	};

	UEAGENTINTERFACE_API FUeAgentTextUnits MeasureText(const FString& Text);

	using FUeAgentMonotonicClock = TFunction<double()>;

	struct FUeAgentOperationDeadlineExceeded
	{
		FString Code;
		FString Stage;
		double DeadlineAt = 0.0;
		double ObservedAt = 0.0;
	};

	class UEAGENTINTERFACE_API FUeAgentOperationDeadline
	{
	public:
		FUeAgentOperationDeadline();
		static FUeAgentOperationDeadline FromTimeout(
			double TimeoutSeconds,
			FUeAgentMonotonicClock Clock);

		double GetStartedAt() const;
		double GetDeadlineAt() const;
		double RemainingSeconds() const;
		bool CanContinue() const;
		bool TryCheckpoint(
			const FString& Stage,
			FUeAgentOperationDeadlineExceeded& OutError) const;

	private:
		FUeAgentOperationDeadline(
			double InStartedAt,
			double InDeadlineAt,
			FUeAgentMonotonicClock InClock);

		double StartedAt;
		double DeadlineAt;
		FUeAgentMonotonicClock Clock;
	};

	struct FUeAgentCancellationRequest
	{
		FString Reason;
		double RequestedAt = 0.0;
	};

	using FUeAgentCancellationRequested = Contract::FDiagnostic;

	class FUeAgentCancellationState;

	class UEAGENTINTERFACE_API FUeAgentCancellationToken
	{
	public:
		bool IsCancelled() const;
		const FUeAgentCancellationRequest* GetRequest() const;
		bool TryCheckpoint(FUeAgentCancellationRequested& OutError) const;

	private:
		explicit FUeAgentCancellationToken(
			TSharedRef<FUeAgentCancellationState, ESPMode::ThreadSafe> InState);

		TSharedRef<FUeAgentCancellationState, ESPMode::ThreadSafe> State;

		friend class FUeAgentCancellationSource;
	};

	class UEAGENTINTERFACE_API FUeAgentCancellationSource
	{
	public:
		explicit FUeAgentCancellationSource(FUeAgentMonotonicClock Clock);

		FUeAgentCancellationToken GetToken() const;
		const FUeAgentCancellationRequest& Request(const FString& Reason);

	private:
		TSharedRef<FUeAgentCancellationState, ESPMode::ThreadSafe> State;
		FUeAgentMonotonicClock Clock;
	};
}

#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"

class FJsonValue;

namespace UE::UAI::ContextResource
{
	class FUeAgentResourceTracker;
}

namespace UE::UAI::StateSemantic
{
	class FUeAgentCanonicalValue;

	namespace CanonicalJson
	{
		/** Comparison yields equivalence, the first semantic difference, or its
		 * original failure diagnostic. An invalid value pointer in a difference
		 * denotes absence, distinct from an explicit JSON null. */
		struct UEAGENTINTERFACE_API FValueComparison
		{
			bool bEquivalent = true;
			FString Path;
			TSharedPtr<FJsonValue> ExpectedValue;
			TSharedPtr<FJsonValue> ActualValue;
			TSharedPtr<const UE::UAI::Contract::FDiagnostic> ComparisonFailure;

			static FValueComparison Different(FString Path,
				TSharedPtr<FJsonValue> Expected, TSharedPtr<FJsonValue> Actual);
			static FValueComparison Failed(UE::UAI::Contract::FDiagnostic Diagnostic);
			bool IsComparable() const { return !ComparisonFailure.IsValid(); }
			FValueComparison WithPrefix(const FString& Prefix) const;
			void ApplyTo(UE::UAI::Contract::FDiagnostic& Diagnostic) const;
		};

		/** A declared-type owner may decide a leaf; unset retains exact document semantics. */
		using FDeclaredValueComparison = TFunction<TOptional<FValueComparison>(
			const TSharedPtr<FJsonValue>&, const TSharedPtr<FJsonValue>&, const FString&)>;
		/** Retains exact JSON number text; native double values use round-trip precision. */
		UEAGENTINTERFACE_API bool TryGetNumberLexeme(const FJsonValue& Value, FString& OutLexeme);

		/** Compares numeric values without rounding exact number strings through double. */
		UEAGENTINTERFACE_API bool AreNumbersEquivalent(const FJsonValue& Left, const FJsonValue& Right);

		/** JSON value semantics: exact numbers, case-sensitive strings/keys, ordered arrays.
		 * Declared comparisons return relative paths; traversal prefixes them once. */
		UEAGENTINTERFACE_API FValueComparison CompareJsonValues(
			const TSharedPtr<FJsonValue>& Left,
			const TSharedPtr<FJsonValue>& Right,
			const FString& RootPath = FString(),
			const FDeclaredValueComparison* DeclaredComparison = nullptr);

		/** Explicit string/key case policy; the original entry retains case-sensitive semantics. */
		UEAGENTINTERFACE_API FValueComparison CompareJsonValuesWithCase(
			const TSharedPtr<FJsonValue>& Left, const TSharedPtr<FJsonValue>& Right,
			ESearchCase::Type CaseSensitivity, const FString& RootPath = FString(),
			const FDeclaredValueComparison* DeclaredComparison = nullptr);

		/** Predicate-only consumers share the structured comparison implementation. */
		UEAGENTINTERFACE_API bool AreJsonValuesEquivalent(
			const TSharedPtr<FJsonValue>& Left, const TSharedPtr<FJsonValue>& Right);

		/** True only when the current JSON has the same generic canonical
		 * projection, including numeric kind and exact retained spelling.
		 * This is a material reuse proof, not a native observation or binding. */
		UEAGENTINTERFACE_API bool MatchesCanonicalProjection(
			const TSharedPtr<FJsonValue>& Current, const FUeAgentCanonicalValue& Previous);

		/** Projects only a typed leaf after its complete allocation preflight succeeds. */
		UEAGENTINTERFACE_API bool TryProjectTypedLeaf(
			const FUeAgentCanonicalValue& Value,
			TFunctionRef<bool(int64)> ChargeMemory,
			TSharedPtr<FJsonValue>& OutValue,
			UE::UAI::Contract::FDiagnostic& OutError);
		/** Same scalar producer as generic conversion, without graph discovery
		 * for an already-selected non-container leaf. Never retains the input. */
		UEAGENTINTERFACE_API bool TryFromJsonScalarValue(
			const FJsonValue& Value,
			UE::UAI::ContextResource::FUeAgentResourceTracker& Resources,
			TSharedPtr<const FUeAgentCanonicalValue>& OutValue,
			UE::UAI::Contract::FDiagnostic& OutError);

		UEAGENTINTERFACE_API bool TryFromJsonValue(
			const TSharedPtr<FJsonValue>& Value,
			UE::UAI::ContextResource::FUeAgentResourceTracker& Resources,
			TSharedPtr<const FUeAgentCanonicalValue>& OutValue,
			UE::UAI::Contract::FDiagnostic& OutError);

		UEAGENTINTERFACE_API bool TryToJsonValue(
			const FUeAgentCanonicalValue& Value,
			TSharedPtr<FJsonValue>& OutValue,
			UE::UAI::Contract::FDiagnostic& OutError);

		/** Codec input that will be projected back to canonical values: preserve the
		 * integer/decimal distinction even for integral decimal spellings. */
		UEAGENTINTERFACE_API bool TryToAuthoringCodecJson(
			const FUeAgentCanonicalValue& Value, TSharedPtr<FJsonValue>& OutValue,
			UE::UAI::Contract::FDiagnostic& OutError);
		/** Native declaration signatures use the UE JSON writer's original numeric spelling. */
		UEAGENTINTERFACE_API bool TryToAuthoringDeclarationJson(
			const FUeAgentCanonicalValue& Value, TSharedPtr<FJsonValue>& OutValue,
			UE::UAI::Contract::FDiagnostic& OutError);
		UEAGENTINTERFACE_API bool NeedsDecimalPoint(const FString& DecimalLexeme);
	}
}

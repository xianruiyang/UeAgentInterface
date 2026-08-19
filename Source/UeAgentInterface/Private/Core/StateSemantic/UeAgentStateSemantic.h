#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentExecutionContextPolicy.h"
#include "Core/ContextResource/UeAgentContextResource.h"

namespace UE::UAI::StateSemantic
{
	using UE::UAI::Contract::EUeAgentStatePlane;
	using UE::UAI::ContextResource::EUeAgentResourceDimension;
	using UE::UAI::ContextResource::FUeAgentResourceTracker;

	enum class EUeAgentValueKind : uint8
	{
		Null,
		Boolean,
		String,
		Integer,
		Decimal,
		Array,
		Object,
		Temporal,
		Spatial,
	};

	enum class EUeAgentDefaultSourceKind : uint8
	{
		Factory,
		ClassDefaultObject,
		Parent,
		Constructor,
		StructInitializer,
		Schema,
		DomainNative,
		PostCreateReadback,
	};

	enum class EUeAgentAuthoredPresence : uint8
	{
		Absent,
		Present,
	};

	enum class EUeAgentOverrideState : uint8
	{
		None,
		Inherited,
		Overridden,
	};

	enum class EUeAgentSpatialQuantityKind : uint8
	{
		Point,
		Direction,
		Normal,
		Transform,
	};

	enum class EUeAgentDifferenceKind : uint8
	{
		ValueMismatch,
		TypeMismatch,
		IdentityMismatch,
		DefaultMismatch,
		DetailsMismatch,
		AuthoredPresenceMismatch,
		OverrideStateMismatch,
		StatePlaneMismatch,
		SnapshotRevisionMismatch,
		TimeDomainMismatch,
		TimeStateMismatch,
		FrameMismatch,
		SpatialQuantityMismatch,
		SubjectMismatch,
		MissingLeft,
		MissingRight,
	};

	struct UEAGENTINTERFACE_API FUeAgentStateSemanticError
	{
		FString Code;
		FString Path;
		FString Detail;
		EUeAgentResourceDimension ResourceDimension =
			EUeAgentResourceDimension::WorkUnits;
		int64 ResourceLimit = 0;
		int64 ResourceObserved = 0;
	};

	class FUeAgentCanonicalValue;
	class FUeAgentSpatialValue;
	class FUeAgentCanonicalSnapshot;

	using FUeAgentCanonicalValuePtr = TSharedPtr<const FUeAgentCanonicalValue>;
	using FUeAgentCanonicalValueRef = TSharedRef<const FUeAgentCanonicalValue>;
	using FUeAgentSpatialValuePtr = TSharedPtr<const FUeAgentSpatialValue>;
	using FUeAgentSpatialValueRef = TSharedRef<const FUeAgentSpatialValue>;
	using FUeAgentCanonicalSnapshotPtr = TSharedPtr<const FUeAgentCanonicalSnapshot>;
	using FUeAgentCanonicalSnapshotRef = TSharedRef<const FUeAgentCanonicalSnapshot>;

	class UEAGENTINTERFACE_API FUeAgentTemporalPoint final
	{
	public:
		FUeAgentTemporalPoint(
			FString InClockId,
			FString InTick,
			FString InRateNumerator = TEXT("1"),
			FString InRateDenominator = TEXT("1"),
			FString InState = TEXT("captured"));

		const FString& GetClockId() const;
		const FString& GetTick() const;
		const FString& GetRateNumerator() const;
		const FString& GetRateDenominator() const;
		const FString& GetState() const;
		bool IsValid() const;
		bool operator==(const FUeAgentTemporalPoint& Other) const;

	private:
		FString ClockId;
		FString Tick;
		FString RateNumerator;
		FString RateDenominator;
		FString State;
		bool bValid = false;
	};

	UEAGENTINTERFACE_API bool AreTemporalPointsEquivalent(
		const FUeAgentTemporalPoint& Left,
		const FUeAgentTemporalPoint& Right);

	class UEAGENTINTERFACE_API FUeAgentSpatialFrame final
	{
	public:
		FUeAgentSpatialFrame(
			FString InFrameId,
			FString InBasisFrameId,
			int32 InDimension,
			TArray<TArray<FString>> InBasis,
			TArray<FString> InOrigin,
			FString InUnit,
			FString InBasisEvidence,
			FUeAgentTemporalPoint InObservedAt);

		const FString& GetFrameId() const;
		const FString& GetBasisFrameId() const;
		int32 GetDimension() const;
		const TArray<TArray<FString>>& GetBasis() const;
		const TArray<FString>& GetOrigin() const;
		const FString& GetUnit() const;
		const FString& GetBasisEvidence() const;
		const FUeAgentTemporalPoint& GetObservedAt() const;
		bool IsValid() const;
		bool SemanticallyEquals(const FUeAgentSpatialFrame& Other) const;

	private:
		FString FrameId;
		FString BasisFrameId;
		int32 Dimension = 0;
		TArray<TArray<FString>> Basis;
		TArray<FString> Origin;
		FString Unit;
		FString BasisEvidence;
		FUeAgentTemporalPoint ObservedAt;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API FUeAgentSpatialConversion final
	{
	public:
		FUeAgentSpatialConversion(
			FString InConversionId,
			FString InFromFrame,
			FString InToFrame,
			int32 InSourceDimension,
			int32 InTargetDimension,
			TArray<TArray<FString>> InLinear,
			TArray<FString> InTranslation,
			FUeAgentTemporalPoint InValidAt,
			FString InEvidence);

		const FString& GetConversionId() const;
		const FString& GetFromFrame() const;
		const FString& GetToFrame() const;
		int32 GetSourceDimension() const;
		int32 GetTargetDimension() const;
		const TArray<TArray<FString>>& GetLinear() const;
		const TArray<FString>& GetTranslation() const;
		const FUeAgentTemporalPoint& GetValidAt() const;
		const FString& GetEvidence() const;
		bool IsValid() const;

	private:
		FString ConversionId;
		FString FromFrame;
		FString ToFrame;
		int32 SourceDimension = 0;
		int32 TargetDimension = 0;
		TArray<TArray<FString>> Linear;
		TArray<FString> Translation;
		FUeAgentTemporalPoint ValidAt;
		FString Evidence;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API FUeAgentSpatialValue final
	{
	public:
		FUeAgentSpatialValue(
			FString InSubject,
			EUeAgentSpatialQuantityKind InQuantityKind,
			FUeAgentSpatialFrame InFrame,
			TArray<FString> InComponents,
			FUeAgentTemporalPoint InObservedAt,
			TArray<FString> InConversionEvidence = {});

		const FString& GetSubject() const;
		EUeAgentSpatialQuantityKind GetQuantityKind() const;
		const FUeAgentSpatialFrame& GetFrame() const;
		const TArray<FString>& GetComponents() const;
		const FUeAgentTemporalPoint& GetObservedAt() const;
		const TArray<FString>& GetConversionEvidence() const;
		bool IsValid() const;
		bool SemanticallyEquals(const FUeAgentSpatialValue& Other) const;

	private:
		FString Subject;
		EUeAgentSpatialQuantityKind QuantityKind =
			EUeAgentSpatialQuantityKind::Point;
		FUeAgentSpatialFrame Frame;
		TArray<FString> Components;
		FUeAgentTemporalPoint ObservedAt;
		TArray<FString> ConversionEvidence;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API FUeAgentCanonicalValue final
	{
	public:
		static bool TryNull(
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentStateSemanticError& OutError);
		static bool TryBoolean(
			bool Value,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentStateSemanticError& OutError);
		static bool TryString(
			const FString& Value,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentStateSemanticError& OutError);
		static bool TryInteger(
			const FString& Lexeme,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentStateSemanticError& OutError);
		static bool TryDecimal(
			const FString& Lexeme,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentStateSemanticError& OutError);
		static bool TryArray(
			const TArray<FUeAgentCanonicalValueRef>& Items,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentStateSemanticError& OutError);
		static bool TryObject(
			const TArray<TPair<FString, FUeAgentCanonicalValueRef>>& Items,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentStateSemanticError& OutError);
		static bool TryTemporal(
			const FUeAgentTemporalPoint& Value,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentStateSemanticError& OutError);
		static bool TrySpatial(
			const FUeAgentSpatialValue& Value,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentStateSemanticError& OutError);

		EUeAgentValueKind GetKind() const;
		bool GetBoolean() const;
		const FString& GetText() const;
		const FString& GetLexeme() const;
		const TArray<FUeAgentCanonicalValueRef>& GetArrayItems() const;
		const TArray<TPair<FString, FUeAgentCanonicalValueRef>>& GetObjectItems() const;
		const TSharedPtr<const FUeAgentTemporalPoint>& GetTemporal() const;
		const FUeAgentSpatialValuePtr& GetSpatial() const;

	private:
		explicit FUeAgentCanonicalValue(EUeAgentValueKind InKind);

		EUeAgentValueKind Kind = EUeAgentValueKind::Null;
		bool bBoolean = false;
		FString Text;
		TArray<FUeAgentCanonicalValueRef> ArrayItems;
		TArray<TPair<FString, FUeAgentCanonicalValueRef>> ObjectItems;
		TSharedPtr<const FUeAgentTemporalPoint> Temporal;
		FUeAgentSpatialValuePtr Spatial;
	};

	class UEAGENTINTERFACE_API FUeAgentCanonicalIdentity final
	{
	public:
		FUeAgentCanonicalIdentity(
			FString InSemanticId,
			FString InTargetKind,
			FString InOwnerId,
			FString InSelector,
			FString InObjectRole,
			FString InGeneration);
		FUeAgentCanonicalIdentity(
			FString InSemanticId,
			FString InTargetKind,
			FString InOwnerId,
			TMap<FString, FString> InSelectors,
			FString InObjectRole,
			FString InGeneration);

		const FString& GetSemanticId() const;
		const FString& GetTargetKind() const;
		const FString& GetOwnerId() const;
		const FString& GetSelector() const;
		FString GetSelector(const FString& Name) const;
		const TMap<FString, FString>& GetSelectors() const;
		const FString& GetObjectRole() const;
		const FString& GetGeneration() const;
		const FString& GetCanonicalKey() const;
		bool IsValid() const;
		bool operator==(const FUeAgentCanonicalIdentity& Other) const;

	private:
		FString SemanticId;
		FString TargetKind;
		FString OwnerId;
		FString Selector;
		TMap<FString, FString> Selectors;
		FString ObjectRole;
		FString Generation;
		FString CanonicalKey;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API FUeAgentCanonicalType final
	{
	public:
		FUeAgentCanonicalType(
			FString InTypeId,
			EUeAgentValueKind InValueKind,
			bool bInNullable,
			FString InContainerKind);

		const FString& GetTypeId() const;
		EUeAgentValueKind GetValueKind() const;
		bool IsNullable() const;
		const FString& GetContainerKind() const;
		bool IsValid() const;
		bool operator==(const FUeAgentCanonicalType& Other) const;

	private:
		FString TypeId;
		EUeAgentValueKind ValueKind = EUeAgentValueKind::Null;
		bool bNullable = false;
		FString ContainerKind;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API FUeAgentDefaultValue final
	{
	public:
		FUeAgentDefaultValue(
			EUeAgentDefaultSourceKind InSourceKind,
			FString InSourceIdentity,
			FString InProvenance,
			FUeAgentCanonicalValueRef InValue);

		EUeAgentDefaultSourceKind GetSourceKind() const;
		const FString& GetSourceIdentity() const;
		const FString& GetProvenance() const;
		const FUeAgentCanonicalValueRef& GetValue() const;
		bool IsValid() const;

	private:
		EUeAgentDefaultSourceKind SourceKind =
			EUeAgentDefaultSourceKind::Factory;
		FString SourceIdentity;
		FString Provenance;
		FUeAgentCanonicalValueRef Value;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API FUeAgentStateDetails final
	{
	public:
		FUeAgentStateDetails(
			FString InDisplayName,
			FString InTooltip,
			TArray<FString> InCategoryPath,
			bool bInEditable,
			FString InAvailability,
			TMap<FString, FString> InConstraints);

		const FString& GetDisplayName() const;
		const FString& GetTooltip() const;
		const TArray<FString>& GetCategoryPath() const;
		bool IsEditable() const;
		const FString& GetAvailability() const;
		const TMap<FString, FString>& GetConstraints() const;
		bool IsValid() const;
		bool operator==(const FUeAgentStateDetails& Other) const;

	private:
		FString DisplayName;
		FString Tooltip;
		TArray<FString> CategoryPath;
		bool bEditable = false;
		FString Availability;
		TMap<FString, FString> Constraints;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API FUeAgentStateProvenance final
	{
	public:
		FUeAgentStateProvenance(
			FString InSourceKind,
			FString InSourceId,
			FString InEvidenceId);

		const FString& GetSourceKind() const;
		const FString& GetSourceId() const;
		const FString& GetEvidenceId() const;
		bool IsValid() const;
		bool operator==(const FUeAgentStateProvenance& Other) const;

	private:
		FString SourceKind;
		FString SourceId;
		FString EvidenceId;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API FUeAgentStateRevision final
	{
	public:
		FUeAgentStateRevision(
			FString InSourceId,
			FString InGeneration,
			int64 InSequence);

		const FString& GetSourceId() const;
		const FString& GetGeneration() const;
		int64 GetSequence() const;
		bool IsValid() const;
		bool operator==(const FUeAgentStateRevision& Other) const;
		bool operator!=(const FUeAgentStateRevision& Other) const;

	private:
		FString SourceId;
		FString Generation;
		int64 Sequence = -1;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API FUeAgentStateEntry final
	{
	public:
		FUeAgentStateEntry(
			FUeAgentCanonicalIdentity InIdentity,
			FUeAgentCanonicalType InType,
			FUeAgentCanonicalValueRef InValue,
			FUeAgentDefaultValue InDefault,
			FUeAgentStateDetails InDetails,
			FUeAgentStateRevision InObservedRevision,
			EUeAgentAuthoredPresence InAuthoredPresence =
				EUeAgentAuthoredPresence::Absent,
			EUeAgentOverrideState InOverrideState =
				EUeAgentOverrideState::None);

		const FUeAgentCanonicalIdentity& GetIdentity() const;
		const FUeAgentCanonicalType& GetType() const;
		const FUeAgentCanonicalValueRef& GetValue() const;
		const FUeAgentDefaultValue& GetDefault() const;
		const FUeAgentStateDetails& GetDetails() const;
		const FUeAgentStateRevision& GetObservedRevision() const;
		EUeAgentAuthoredPresence GetAuthoredPresence() const;
		EUeAgentOverrideState GetOverrideState() const;
		bool IsValid() const;
		FUeAgentStateEntry WithObservedRevision(
			FUeAgentStateRevision Revision) const;

	private:
		FUeAgentCanonicalIdentity Identity;
		FUeAgentCanonicalType Type;
		FUeAgentCanonicalValueRef Value;
		FUeAgentDefaultValue Default;
		FUeAgentStateDetails Details;
		FUeAgentStateRevision ObservedRevision;
		EUeAgentAuthoredPresence AuthoredPresence =
			EUeAgentAuthoredPresence::Absent;
		EUeAgentOverrideState OverrideState =
			EUeAgentOverrideState::None;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API FUeAgentCanonicalSnapshot final
	{
	public:
		static bool TryCreate(
			const FString& SnapshotId,
			EUeAgentStatePlane StatePlane,
			const FUeAgentStateRevision& Revision,
			const FUeAgentTemporalPoint& CapturedAt,
			const FUeAgentStateProvenance& Provenance,
			const TArray<FUeAgentStateEntry>& Entries,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalSnapshotPtr& OutSnapshot,
			FUeAgentStateSemanticError& OutError);
		static bool TryCreateExpected(
			const FString& SnapshotId,
			EUeAgentStatePlane StatePlane,
			const FUeAgentStateRevision& Revision,
			const FUeAgentTemporalPoint& CapturedAt,
			const FUeAgentStateProvenance& Provenance,
			const TArray<FUeAgentStateEntry>& Entries,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalSnapshotPtr& OutSnapshot,
			FUeAgentStateSemanticError& OutError);

		const FString& GetSnapshotId() const;
		EUeAgentStatePlane GetStatePlane() const;
		const FUeAgentStateRevision& GetRevision() const;
		const FUeAgentTemporalPoint& GetCapturedAt() const;
		const FUeAgentStateProvenance& GetProvenance() const;
		const TArray<FUeAgentStateEntry>& GetEntries() const;

	private:
		static bool TryCreateWithRequiredProvenance(
			const FString& SnapshotId,
			EUeAgentStatePlane StatePlane,
			const FUeAgentStateRevision& Revision,
			const FUeAgentTemporalPoint& CapturedAt,
			const FUeAgentStateProvenance& Provenance,
			const FString& RequiredSourceKind,
			const FString& ProvenanceErrorCode,
			const TArray<FUeAgentStateEntry>& Entries,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalSnapshotPtr& OutSnapshot,
			FUeAgentStateSemanticError& OutError);

		FUeAgentCanonicalSnapshot(
			FString InSnapshotId,
			EUeAgentStatePlane InStatePlane,
			FUeAgentStateRevision InRevision,
			FUeAgentTemporalPoint InCapturedAt,
			FUeAgentStateProvenance InProvenance,
			TArray<FUeAgentStateEntry> InEntries);

		FString SnapshotId;
		EUeAgentStatePlane StatePlane = EUeAgentStatePlane::AuthoringMemory;
		FUeAgentStateRevision Revision;
		FUeAgentTemporalPoint CapturedAt;
		FUeAgentStateProvenance Provenance;
		TArray<FUeAgentStateEntry> Entries;
	};

	class UEAGENTINTERFACE_API FUeAgentSemanticDifference final
	{
	public:
		FUeAgentSemanticDifference(
			EUeAgentDifferenceKind InKind,
			FString InPath,
			FUeAgentCanonicalValuePtr InLeft,
			FUeAgentCanonicalValuePtr InRight);

		EUeAgentDifferenceKind GetKind() const;
		const FString& GetPath() const;
		const FUeAgentCanonicalValuePtr& GetLeft() const;
		const FUeAgentCanonicalValuePtr& GetRight() const;

	private:
		EUeAgentDifferenceKind Kind = EUeAgentDifferenceKind::ValueMismatch;
		FString Path;
		FUeAgentCanonicalValuePtr Left;
		FUeAgentCanonicalValuePtr Right;
	};

	class UEAGENTINTERFACE_API FUeAgentSemanticComparison final
	{
	public:
		explicit FUeAgentSemanticComparison(
			TArray<FUeAgentSemanticDifference> InDifferences = {});

		bool IsEquivalent() const;
		const TArray<FUeAgentSemanticDifference>& GetDifferences() const;

	private:
		TArray<FUeAgentSemanticDifference> Differences;
	};

	UEAGENTINTERFACE_API FUeAgentSemanticComparison CompareValues(
		const FUeAgentCanonicalValueRef& Left,
		const FUeAgentCanonicalValueRef& Right,
		const FString& Path,
		FUeAgentResourceTracker& Resources);

	UEAGENTINTERFACE_API bool TryCompareValues(
		const FUeAgentCanonicalValueRef& Left,
		const FUeAgentCanonicalValueRef& Right,
		const FString& Path,
		FUeAgentResourceTracker& Resources,
		FUeAgentSemanticComparison& OutComparison,
		FUeAgentStateSemanticError& OutError);

	UEAGENTINTERFACE_API FUeAgentSemanticComparison CompareSnapshots(
		const FUeAgentCanonicalSnapshotRef& Left,
		const FUeAgentCanonicalSnapshotRef& Right,
		FUeAgentResourceTracker& Resources);

	UEAGENTINTERFACE_API bool TryCompareSnapshots(
		const FUeAgentCanonicalSnapshotRef& Left,
		const FUeAgentCanonicalSnapshotRef& Right,
		FUeAgentResourceTracker& Resources,
		FUeAgentSemanticComparison& OutComparison,
		FUeAgentStateSemanticError& OutError);

	UEAGENTINTERFACE_API bool TryConvertSpatialValue(
		const FUeAgentSpatialValue& Value,
		const FUeAgentSpatialFrame& TargetFrame,
		const FUeAgentSpatialConversion& Conversion,
		FUeAgentResourceTracker& Resources,
		FUeAgentSpatialValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError);
}

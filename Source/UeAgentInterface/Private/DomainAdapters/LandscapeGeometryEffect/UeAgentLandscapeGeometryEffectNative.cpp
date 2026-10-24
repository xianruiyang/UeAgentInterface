#include "DomainAdapters/LandscapeGeometryEffect/UeAgentLandscapeGeometryEffectCapabilities.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Containers/UnrealString.h"

#include "DomainAdapters/Landscape/UeAgentLandscapeHeightState.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringState.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Landscape.h"
#include "LandscapeDataAccess.h"
#include "LandscapeEdit.h"
#include "LandscapeInfo.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "UObject/Package.h"

namespace UE::UAI::DomainAdapters::LandscapeGeometryEffect
{
	using namespace ContextResource;
	using namespace ExecutionMutation;
	using namespace GenericEffect;
	using namespace PlanningPreflight;
	using namespace StateSemantic;

	namespace
	{
		enum class ELandscapeGeometryActionKind : uint8
		{
			Create,
			HeightDelta,
		};

		struct FLandscapeGeometryAction
		{
			ELandscapeGeometryActionKind Kind =
				ELandscapeGeometryActionKind::Create;
			FString CanonicalKey;
			FString TargetRef;
			int32 QuadsPerSection = 0;
			int32 SectionsPerComponent = 0;
			int32 ComponentCountX = 0;
			int32 ComponentCountY = 0;
			int32 MinX = 0;
			int32 MinY = 0;
			int32 MaxX = 0;
			int32 MaxY = 0;
			int32 RowStride = 0;
			FVector Translation = FVector::ZeroVector;
			FQuat Rotation = FQuat::Identity;
			FVector Scale = FVector::OneVector;
			float InitialLocalHeight = 0.0f;
			TArray<float> DeltaLocalHeights;
			TArray<uint16> OriginalEncodedHeights;
			TWeakObjectPtr<ALandscape> Landscape;
			TWeakObjectPtr<UPackage> Package;
			bool bPackageWasDirty = false;
		};

		bool Fail(UE::UAI::Contract::FDiagnostic& OutError, UE::UAI::Contract::FDiagnostic Diagnostic)
		{
			OutError = MoveTemp(Diagnostic);
			return false;
		}

		FUeAgentCanonicalValuePtr FindField(
			const FUeAgentCanonicalValuePtr& Object,
			const FString& Field)
		{
			if (!Object.IsValid()
				|| Object->GetKind() != EUeAgentValueKind::Object)
			{
				return nullptr;
			}
			for (const TPair<
				FString,
				FUeAgentCanonicalValueRef>& Item
					: Object->GetObjectItems())
			{
				if (Item.Key == Field)
				{
					return FUeAgentCanonicalValuePtr(Item.Value);
				}
			}
			return nullptr;
		}

		FString ValueKindName(EUeAgentValueKind Kind)
		{
			switch (Kind)
			{
			case EUeAgentValueKind::Null: return TEXT("null");
			case EUeAgentValueKind::Boolean: return TEXT("boolean");
			case EUeAgentValueKind::String: return TEXT("string");
			case EUeAgentValueKind::Integer: return TEXT("integer");
			case EUeAgentValueKind::Decimal: return TEXT("decimal");
			case EUeAgentValueKind::Array: return TEXT("array");
			case EUeAgentValueKind::Object: return TEXT("object");
			case EUeAgentValueKind::Temporal: return TEXT("temporal");
			case EUeAgentValueKind::Spatial: return TEXT("spatial");
			default: return FString::FromInt(static_cast<int32>(Kind));
			}
		}

		FExecutionMutationError NativeValueFailure(FString Code, FString Reason, FString Field,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected, FString Target = {})
		{
			auto Error = FExecutionMutationError::Failure(MoveTemp(Code), MoveTemp(Reason), TEXT("execution"));
			Error.SourceKind = TEXT("native_result");
			Error.TargetPath = MoveTemp(Target);
			Error.PropertyPath = MoveTemp(Field);
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			Error.ActualValuePresent = Error.ActualValue.IsValid();
			Error.ExpectedValuePresent = Error.ExpectedValue.IsValid();
			return Error;
		}

		FExecutionMutationError NativeConditionFailure(FString Code, FString Reason, FString Field, FString Target = {})
		{
			return NativeValueFailure(MoveTemp(Code), MoveTemp(Reason), MoveTemp(Field),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), MoveTemp(Target));
		}

		TSharedPtr<FJsonValue> NumberArrayValue(const TArray<double>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> Items;
			for (const double Value : Values) Items.Add(MakeShared<FJsonValueNumber>(Value));
			return MakeShared<FJsonValueArray>(Items);
		}

		TSharedPtr<FJsonValue> IntegerPairValue(int32 X, int32 Y)
		{
			return NumberArrayValue({static_cast<double>(X), static_cast<double>(Y)});
		}

		TSharedPtr<FJsonValue> RegionValue(int32 MinX, int32 MinY, int32 MaxX, int32 MaxY)
		{
			auto Value = MakeShared<FJsonObject>();
			Value->SetField(TEXT("min_inclusive"), IntegerPairValue(MinX, MinY));
			Value->SetField(TEXT("max_inclusive"), IntegerPairValue(MaxX, MaxY));
			return MakeShared<FJsonValueObject>(Value);
		}

		bool RequireWriteFrame(const FString& Actual, const FString& Expected, const FString& Target,
			FExecutionMutationError& OutError)
		{
			return Actual == Expected || Fail(OutError, NativeValueFailure(TEXT("landscape_geometry_write_frame_mismatch"),
				TEXT("The native Landscape action uses an incompatible write frame."), TEXT("/write_frame_id"),
				MakeShared<FJsonValueString>(Actual), MakeShared<FJsonValueString>(Expected), Target));
		}

		FExecutionMutationError FieldTypeFailure(const FUeAgentCanonicalValuePtr& Object,
			const FString& Field, const FUeAgentCanonicalValuePtr& Value, const FString& Expected)
		{
			if (!Object.IsValid())
				return NativeConditionFailure(TEXT("landscape_geometry_internal_field_type"),
					TEXT("The native Landscape action payload is missing."), TEXT("/after_present"));
			if (Object->GetKind() != EUeAgentValueKind::Object)
				return NativeValueFailure(TEXT("landscape_geometry_internal_field_type"),
					TEXT("The native Landscape action payload must be an object."), TEXT("/after/kind"),
					MakeShared<FJsonValueString>(ValueKindName(Object->GetKind())), MakeShared<FJsonValueString>(TEXT("object")));
			return NativeValueFailure(TEXT("landscape_geometry_internal_field_type"),
				Value.IsValid() ? TEXT("The native Landscape action field has an incompatible value kind.")
					: TEXT("A required native Landscape action field is missing."), TEXT("/") + Field,
				Value.IsValid() ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(ValueKindName(Value->GetKind()))) : nullptr,
				MakeShared<FJsonValueString>(Expected));
		}

		bool ReadString(const FUeAgentCanonicalValuePtr& Object, const FString& Field,
			FString& OutValue, FExecutionMutationError& OutError)
		{
			OutValue.Reset();
			const auto Value = FindField(Object, Field);
			if (!Value.IsValid() || Value->GetKind() != EUeAgentValueKind::String)
				return Fail(OutError, FieldTypeFailure(Object, Field, Value, TEXT("string")));
			OutValue = Value->GetText();
			return !OutValue.IsEmpty() || Fail(OutError, NativeValueFailure(
				TEXT("landscape_geometry_internal_field_empty"), TEXT("The native Landscape action string must not be empty."),
				TEXT("/") + Field, MakeShared<FJsonValueString>(OutValue), MakeShared<FJsonValueString>(TEXT("non-empty string"))));
		}

		bool ReadNumber(const FUeAgentCanonicalValuePtr& Object, const FString& Field,
			double& OutValue, FExecutionMutationError& OutError)
		{
			OutValue = 0.0;
			const auto Value = FindField(Object, Field);
			if (!Value.IsValid() || (Value->GetKind() != EUeAgentValueKind::Decimal && Value->GetKind() != EUeAgentValueKind::Integer))
				return Fail(OutError, FieldTypeFailure(Object, Field, Value, TEXT("decimal or integer")));
			OutValue = FCString::Atod(*Value->GetLexeme());
			return FMath::IsFinite(OutValue) || Fail(OutError, NativeValueFailure(
				TEXT("landscape_geometry_internal_number_invalid"), TEXT("The native Landscape action number must be finite."),
				TEXT("/") + Field, MakeShared<FJsonValueString>(Value->GetLexeme()), MakeShared<FJsonValueString>(TEXT("finite number"))));
		}

		bool ReadInteger(const FUeAgentCanonicalValuePtr& Object, const FString& Field,
			int32& OutValue, FExecutionMutationError& OutError)
		{
			double Number = 0.0;
			if (!ReadNumber(Object, Field, Number, OutError)) return false;
			if (Number < static_cast<double>(MIN_int32) || Number > static_cast<double>(MAX_int32))
				return Fail(OutError, NativeValueFailure(TEXT("landscape_geometry_internal_integer_invalid"),
					TEXT("The native Landscape action integer is outside the signed 32-bit range."), TEXT("/") + Field,
					MakeShared<FJsonValueNumber>(Number), MakeShared<FJsonValueString>(TEXT("integer in [-2147483648, 2147483647]"))));
			OutValue = FMath::RoundToInt(Number);
			return FMath::IsNearlyEqual(Number, static_cast<double>(OutValue), UE_DOUBLE_SMALL_NUMBER)
				|| Fail(OutError, NativeValueFailure(TEXT("landscape_geometry_internal_integer_invalid"),
					TEXT("The native Landscape action value must be an integer."), TEXT("/") + Field,
					MakeShared<FJsonValueNumber>(Number), MakeShared<FJsonValueString>(TEXT("integer"))));
		}

		bool ReadNumberArray(const FUeAgentCanonicalValuePtr& Object, const FString& Field,
			const int32 ExpectedCount, TArray<double>& OutValues, FExecutionMutationError& OutError)
		{
			OutValues.Reset();
			const auto Value = FindField(Object, Field);
			if (!Value.IsValid() || Value->GetKind() != EUeAgentValueKind::Array)
				return Fail(OutError, FieldTypeFailure(Object, Field, Value, TEXT("array")));
			if (ExpectedCount >= 0 && Value->GetArrayItems().Num() != ExpectedCount)
				return Fail(OutError, NativeValueFailure(TEXT("landscape_geometry_internal_field_type"),
					TEXT("The native Landscape action array has an unexpected number of elements."), TEXT("/") + Field + TEXT("/count"),
					MakeShared<FJsonValueNumber>(Value->GetArrayItems().Num()), MakeShared<FJsonValueNumber>(ExpectedCount)));
			for (int32 Index = 0; Index < Value->GetArrayItems().Num(); ++Index)
			{
				const auto& Item = Value->GetArrayItems()[Index];
				const FString Path = TEXT("/") + Field + TEXT("/") + FString::FromInt(Index);
				if (Item->GetKind() != EUeAgentValueKind::Decimal && Item->GetKind() != EUeAgentValueKind::Integer)
					return Fail(OutError, NativeValueFailure(TEXT("landscape_geometry_internal_number_invalid"),
						TEXT("The native Landscape action array element must be numeric."), Path,
						MakeShared<FJsonValueString>(ValueKindName(Item->GetKind())), MakeShared<FJsonValueString>(TEXT("decimal or integer"))));
				const double Number = FCString::Atod(*Item->GetLexeme());
				if (!FMath::IsFinite(Number))
					return Fail(OutError, NativeValueFailure(TEXT("landscape_geometry_internal_number_invalid"),
						TEXT("The native Landscape action array element must be finite."), Path,
						MakeShared<FJsonValueString>(Item->GetLexeme()), MakeShared<FJsonValueString>(TEXT("finite number"))));
				OutValues.Add(Number);
			}
			return true;
		}

		bool ReadIntegerArray(const FUeAgentCanonicalValuePtr& Object, const FString& Field,
			const int32 ExpectedCount, TArray<int32>& OutValues, FExecutionMutationError& OutError)
		{
			TArray<double> Numbers;
			if (!ReadNumberArray(Object, Field, ExpectedCount, Numbers, OutError)) return false;
			OutValues.Reset();
			for (int32 Index = 0; Index < Numbers.Num(); ++Index)
			{
				const double Number = Numbers[Index];
				const FString Path = TEXT("/") + Field + TEXT("/") + FString::FromInt(Index);
				if (Number < static_cast<double>(MIN_int32) || Number > static_cast<double>(MAX_int32))
					return Fail(OutError, NativeValueFailure(TEXT("landscape_geometry_internal_integer_invalid"),
						TEXT("The native Landscape action array integer is outside the signed 32-bit range."), Path,
						MakeShared<FJsonValueNumber>(Number), MakeShared<FJsonValueString>(TEXT("integer in [-2147483648, 2147483647]"))));
				const int32 Integer = FMath::RoundToInt(Number);
				if (!FMath::IsNearlyEqual(Number, static_cast<double>(Integer), UE_DOUBLE_SMALL_NUMBER))
					return Fail(OutError, NativeValueFailure(TEXT("landscape_geometry_internal_integer_invalid"),
						TEXT("The native Landscape action array element must be an integer."), Path,
						MakeShared<FJsonValueNumber>(Number), MakeShared<FJsonValueString>(TEXT("integer"))));
				OutValues.Add(Integer);
			}
			return true;
		}

		bool IsLandscapeLocalHeightRepresentable(
			const double LocalHeight)
		{
			return FMath::IsFinite(LocalHeight)
				&& LocalHeight >= -256.0
				&& LocalHeight <= 255.9921875;
		}

		bool ReadLiveExtent(ALandscape& Landscape, int32& OutMinX, int32& OutMinY,
			int32& OutMaxX, int32& OutMaxY, FExecutionMutationError& OutError, const FString& Target)
		{
			ULandscapeInfo* Info = Landscape.GetLandscapeInfo();
			if (!Info)
				return Fail(OutError, NativeConditionFailure(TEXT("landscape_geometry_extent_unavailable"),
					TEXT("The native Landscape has no LandscapeInfo."), TEXT("/landscape_info_present"), Target));
			if (!Info->GetLandscapeExtent(OutMinX, OutMinY, OutMaxX, OutMaxY))
				return Fail(OutError, NativeConditionFailure(TEXT("landscape_geometry_extent_unavailable"),
					TEXT("The native Landscape extent could not be read."), TEXT("/extent_available"), Target));
			return true;
		}

		bool PrepareCreate(const FPlannedChange& Change, FLandscapeGeometryAction& OutAction, FExecutionMutationError& OutError)
		{
			if (Change.GetIntent() != EChangeIntent::Create)
				return Fail(OutError, NativeValueFailure(TEXT("landscape_geometry_create_intent_invalid"),
					TEXT("A Landscape create action requires the create intent."), TEXT("/intent"),
					MakeShared<FJsonValueString>(LexToString(Change.GetIntent())), MakeShared<FJsonValueString>(TEXT("create"))));
			const auto& After = Change.GetAfter();
			FString WriteFrame;
			FString ConversionEvidence;
			TArray<int32> ComponentCount, MinInclusive, MaxInclusive;
			TArray<double> Translation, Rotation, Scale;
			double InitialLocalHeight = 0.0;
			if (!ReadString(After, TEXT("target_ref"), OutAction.TargetRef, OutError)
				|| !ReadString(After, TEXT("write_frame_id"), WriteFrame, OutError)
				|| !ReadString(After, TEXT("conversion_evidence"), ConversionEvidence, OutError)
				|| !RequireWriteFrame(WriteFrame, TEXT("ue.landscape.actor_and_local_sample_grid"), OutAction.TargetRef, OutError)
				|| !ReadInteger(After, TEXT("quads_per_section"), OutAction.QuadsPerSection, OutError)
				|| !ReadInteger(After, TEXT("sections_per_component"), OutAction.SectionsPerComponent, OutError)
				|| !ReadIntegerArray(After, TEXT("component_count"), 2, ComponentCount, OutError)
				|| !ReadIntegerArray(After, TEXT("sample_min_inclusive"), 2, MinInclusive, OutError)
				|| !ReadIntegerArray(After, TEXT("sample_max_inclusive"), 2, MaxInclusive, OutError)
				|| !ReadNumberArray(After, TEXT("translation_cm"), 3, Translation, OutError)
				|| !ReadNumberArray(After, TEXT("rotation_quaternion_xyzw"), 4, Rotation, OutError)
				|| !ReadNumberArray(After, TEXT("scale"), 3, Scale, OutError)
				|| !ReadNumber(After, TEXT("initial_local_height"), InitialLocalHeight, OutError))
				return false;
			OutAction.Kind = ELandscapeGeometryActionKind::Create;
			OutAction.CanonicalKey = Change.GetIdentity().GetCanonicalKey();
			OutAction.ComponentCountX = ComponentCount[0];
			OutAction.ComponentCountY = ComponentCount[1];
			OutAction.MinX = MinInclusive[0]; OutAction.MinY = MinInclusive[1];
			OutAction.MaxX = MaxInclusive[0]; OutAction.MaxY = MaxInclusive[1];
			OutAction.Translation = FVector(Translation[0], Translation[1], Translation[2]);
			OutAction.Rotation = FQuat(Rotation[0], Rotation[1], Rotation[2], Rotation[3]);
			OutAction.Scale = FVector(Scale[0], Scale[1], Scale[2]);
			OutAction.InitialLocalHeight = static_cast<float>(InitialLocalHeight);
			const FString Code = TEXT("landscape_geometry_create_preflight_invalid");
			if (OutAction.QuadsPerSection <= 0)
				return Fail(OutError, NativeValueFailure(Code, TEXT("Landscape quads per section must be positive."), TEXT("/quads_per_section"),
					MakeShared<FJsonValueNumber>(OutAction.QuadsPerSection), MakeShared<FJsonValueString>(TEXT("> 0")), OutAction.TargetRef));
			if (OutAction.SectionsPerComponent != 1 && OutAction.SectionsPerComponent != 2)
				return Fail(OutError, NativeValueFailure(Code, TEXT("Landscape sections per component must be 1 or 2."), TEXT("/sections_per_component"),
					MakeShared<FJsonValueNumber>(OutAction.SectionsPerComponent), MakeShared<FJsonValueString>(TEXT("1 or 2")), OutAction.TargetRef));
			for (int32 Index = 0; Index < ComponentCount.Num(); ++Index)
				if (ComponentCount[Index] <= 0)
					return Fail(OutError, NativeValueFailure(Code, TEXT("Landscape component counts must be positive."),
						TEXT("/component_count/") + FString::FromInt(Index), MakeShared<FJsonValueNumber>(ComponentCount[Index]),
						MakeShared<FJsonValueString>(TEXT("> 0")), OutAction.TargetRef));
			// Sections are now known to be 1 or 2, so both signed 32-bit factors fit int64.
			const int64 ExpectedQuadsX = static_cast<int64>(OutAction.QuadsPerSection) * OutAction.SectionsPerComponent * OutAction.ComponentCountX;
			const int64 ExpectedQuadsY = static_cast<int64>(OutAction.QuadsPerSection) * OutAction.SectionsPerComponent * OutAction.ComponentCountY;
			if (static_cast<int64>(OutAction.MaxX) - OutAction.MinX != ExpectedQuadsX)
				return Fail(OutError, NativeValueFailure(Code, TEXT("The Landscape X sample extent does not match its topology."), TEXT("/sample_max_inclusive/0"),
					MakeShared<FJsonValueNumber>(OutAction.MaxX), MakeShared<FJsonValueString>(::LexToString(static_cast<int64>(OutAction.MinX) + ExpectedQuadsX)), OutAction.TargetRef));
			if (static_cast<int64>(OutAction.MaxY) - OutAction.MinY != ExpectedQuadsY)
				return Fail(OutError, NativeValueFailure(Code, TEXT("The Landscape Y sample extent does not match its topology."), TEXT("/sample_max_inclusive/1"),
					MakeShared<FJsonValueNumber>(OutAction.MaxY), MakeShared<FJsonValueString>(::LexToString(static_cast<int64>(OutAction.MinY) + ExpectedQuadsY)), OutAction.TargetRef));
			for (int32 Index = 0; Index < Scale.Num(); ++Index)
				if (Scale[Index] <= 0.0)
					return Fail(OutError, NativeValueFailure(Code, TEXT("Landscape scale components must be positive."), TEXT("/scale/") + FString::FromInt(Index),
						MakeShared<FJsonValueNumber>(Scale[Index]), MakeShared<FJsonValueString>(TEXT("> 0")), OutAction.TargetRef));
			if (!OutAction.Rotation.IsNormalized())
				return Fail(OutError, NativeValueFailure(Code, TEXT("The Landscape rotation quaternion must be normalized."), TEXT("/rotation_quaternion_xyzw"),
					NumberArrayValue(Rotation), MakeShared<FJsonValueString>(TEXT("normalized quaternion")), OutAction.TargetRef));
			if (!IsLandscapeLocalHeightRepresentable(InitialLocalHeight))
				return Fail(OutError, NativeValueFailure(Code, TEXT("The initial Landscape height is outside its native representable range."), TEXT("/initial_local_height"),
					MakeShared<FJsonValueNumber>(InitialLocalHeight), NumberArrayValue({-256.0, 255.9921875}), OutAction.TargetRef));
			return true;
		}

		bool PrepareHeightDelta(
			UWorld& World,
			IUeAgentLandscapeGeometryEffectReferenceResolver&
				Resolver,
			const FPlannedChange& Change,
			FLandscapeGeometryAction& OutAction,
			FExecutionMutationError& OutError)
		{
			if (Change.GetIntent() != EChangeIntent::Update)
				return Fail(OutError, NativeValueFailure(TEXT("landscape_geometry_height_delta_intent_invalid"),
					TEXT("A Landscape height-delta action requires the update intent."), TEXT("/intent"),
					MakeShared<FJsonValueString>(LexToString(Change.GetIntent())), MakeShared<FJsonValueString>(TEXT("update"))));
			const auto& After = Change.GetAfter();
			FString WriteFrame, ConversionEvidence;
			TArray<double> ExpectedScale, DeltaValues;
			TArray<int32> FrozenMin, FrozenMax, RegionMin, RegionMax;
			if (!ReadString(After, TEXT("target_ref"), OutAction.TargetRef, OutError)
				|| !ReadString(After, TEXT("write_frame_id"), WriteFrame, OutError)
				|| !ReadString(After, TEXT("conversion_evidence"), ConversionEvidence, OutError)
				|| !RequireWriteFrame(WriteFrame, TEXT("ue.landscape.local_sample_grid"), OutAction.TargetRef, OutError)
				|| !ReadNumberArray(After, TEXT("expected_scale"), 3, ExpectedScale, OutError)
				|| !ReadIntegerArray(After, TEXT("frozen_extent_min_inclusive"), 2, FrozenMin, OutError)
				|| !ReadIntegerArray(After, TEXT("frozen_extent_max_inclusive"), 2, FrozenMax, OutError)
				|| !ReadIntegerArray(After, TEXT("region_min_inclusive"), 2, RegionMin, OutError)
				|| !ReadIntegerArray(After, TEXT("region_max_inclusive"), 2, RegionMax, OutError)
				|| !ReadInteger(After, TEXT("row_stride"), OutAction.RowStride, OutError)
				|| !ReadNumberArray(After, TEXT("delta_local_heights"), -1, DeltaValues, OutError))
				return false;
			ALandscape* Landscape = nullptr;
			DomainAdapters::Landscape::FLandscapeHeightStateError TargetError;
			if (!DomainAdapters::Landscape::TryResolveLandscapeHeightTarget(World, OutAction.TargetRef, &Resolver, Landscape, TargetError))
				return Fail(OutError, MoveTemp(TargetError));
			int32 LiveMinX = 0, LiveMinY = 0, LiveMaxX = 0, LiveMaxY = 0;
			const FVector LiveScale = Landscape->GetActorScale3D();
			if (!ReadLiveExtent(*Landscape, LiveMinX, LiveMinY, LiveMaxX, LiveMaxY, OutError, OutAction.TargetRef))
			{
				OutError = FExecutionMutationError::WrapFailure(TEXT("landscape_geometry_live_state_stale"), OutError);
				return false;
			}
			const FString StaleCode = TEXT("landscape_geometry_live_state_stale");
			const int32 LiveMin[] = {LiveMinX, LiveMinY};
			const int32 LiveMax[] = {LiveMaxX, LiveMaxY};
			for (int32 Index = 0; Index < 2; ++Index)
				if (LiveMin[Index] != FrozenMin[Index])
					return Fail(OutError, NativeValueFailure(StaleCode, TEXT("The live Landscape extent minimum differs from the frozen state."),
						TEXT("/frozen_extent_min_inclusive/") + FString::FromInt(Index), MakeShared<FJsonValueNumber>(LiveMin[Index]),
						MakeShared<FJsonValueNumber>(FrozenMin[Index]), OutAction.TargetRef));
			for (int32 Index = 0; Index < 2; ++Index)
				if (LiveMax[Index] != FrozenMax[Index])
					return Fail(OutError, NativeValueFailure(StaleCode, TEXT("The live Landscape extent maximum differs from the frozen state."),
						TEXT("/frozen_extent_max_inclusive/") + FString::FromInt(Index), MakeShared<FJsonValueNumber>(LiveMax[Index]),
						MakeShared<FJsonValueNumber>(FrozenMax[Index]), OutAction.TargetRef));
			if (!LiveScale.Equals(FVector(ExpectedScale[0], ExpectedScale[1], ExpectedScale[2]), UE_KINDA_SMALL_NUMBER))
				return Fail(OutError, NativeValueFailure(StaleCode, TEXT("The live Landscape scale differs from the frozen scale beyond the native tolerance."),
					TEXT("/expected_scale"), NumberArrayValue({LiveScale.X, LiveScale.Y, LiveScale.Z}), NumberArrayValue(ExpectedScale), OutAction.TargetRef));
			OutAction.Kind = ELandscapeGeometryActionKind::HeightDelta;
			OutAction.CanonicalKey = Change.GetIdentity().GetCanonicalKey();
			OutAction.MinX = RegionMin[0]; OutAction.MinY = RegionMin[1];
			OutAction.MaxX = RegionMax[0]; OutAction.MaxY = RegionMax[1];
			OutAction.Scale = LiveScale;
			OutAction.Landscape = Landscape;
			OutAction.Package = Landscape->GetOutermost();
			OutAction.bPackageWasDirty = OutAction.Package.IsValid() && OutAction.Package->IsDirty();
			const int64 Width = static_cast<int64>(OutAction.MaxX) - OutAction.MinX + 1;
			const int64 Height = static_cast<int64>(OutAction.MaxY) - OutAction.MinY + 1;
			const FString PreflightCode = TEXT("landscape_geometry_height_delta_preflight_invalid");
			if (Width <= 0 || Height <= 0)
				return Fail(OutError, NativeValueFailure(PreflightCode, TEXT("The height region minimum must not exceed its inclusive maximum."),
					TEXT("/region"), RegionValue(OutAction.MinX, OutAction.MinY, OutAction.MaxX, OutAction.MaxY),
					MakeShared<FJsonValueString>(TEXT("min_inclusive <= max_inclusive")), OutAction.TargetRef));
			if (OutAction.RowStride != Width)
				return Fail(OutError, NativeValueFailure(PreflightCode, TEXT("The height row stride must equal the region width."), TEXT("/row_stride"),
					MakeShared<FJsonValueNumber>(OutAction.RowStride), MakeShared<FJsonValueString>(::LexToString(Width)), OutAction.TargetRef));
			if (OutAction.MinX < LiveMinX || OutAction.MinY < LiveMinY || OutAction.MaxX > LiveMaxX || OutAction.MaxY > LiveMaxY)
				return Fail(OutError, NativeValueFailure(PreflightCode, TEXT("The height region extends outside the live Landscape extent."), TEXT("/region"),
					RegionValue(OutAction.MinX, OutAction.MinY, OutAction.MaxX, OutAction.MaxY), RegionValue(LiveMinX, LiveMinY, LiveMaxX, LiveMaxY), OutAction.TargetRef));
			// Positive Width equals the int32 row stride; the int32 endpoint-derived Height keeps this product within int64.
			const int64 RequiredSamples = Width * Height;
			if (static_cast<int64>(DeltaValues.Num()) != RequiredSamples)
				return Fail(OutError, NativeValueFailure(PreflightCode, TEXT("There must be one height delta for every region sample."),
					TEXT("/delta_local_heights/count"), MakeShared<FJsonValueNumber>(DeltaValues.Num()),
					MakeShared<FJsonValueString>(::LexToString(RequiredSamples)), OutAction.TargetRef));
			ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
			if (!Info)
				return Fail(OutError, NativeConditionFailure(TEXT("landscape_geometry_info_missing"),
					TEXT("The resolved Landscape has no native LandscapeInfo."), TEXT("/landscape_info_present"), OutAction.TargetRef));
			OutAction.OriginalEncodedHeights.SetNumUninitialized(static_cast<int32>(RequiredSamples));
			FLandscapeEditDataInterface Edit(Info);
			Edit.GetHeightDataFast(OutAction.MinX, OutAction.MinY, OutAction.MaxX, OutAction.MaxY,
				OutAction.OriginalEncodedHeights.GetData(), OutAction.RowStride, nullptr);
			OutAction.DeltaLocalHeights.Reserve(DeltaValues.Num());
			for (int32 Index = 0; Index < DeltaValues.Num(); ++Index)
			{
				const double OriginalHeight = LandscapeDataAccess::GetLocalHeight(OutAction.OriginalEncodedHeights[Index]);
				const double ResultHeight = OriginalHeight + DeltaValues[Index];
				if (!IsLandscapeLocalHeightRepresentable(ResultHeight))
				{
					auto Actual = MakeShared<FJsonObject>();
					Actual->SetNumberField(TEXT("original_height"), OriginalHeight);
					Actual->SetNumberField(TEXT("delta"), DeltaValues[Index]);
					if (FMath::IsFinite(ResultHeight)) Actual->SetNumberField(TEXT("result_height"), ResultHeight);
					else Actual->SetStringField(TEXT("result_height"), ::LexToString(ResultHeight));
					return Fail(OutError, NativeValueFailure(TEXT("landscape_geometry_height_delta_overflow"),
						TEXT("Applying this height delta would exceed the native Landscape height range."),
						TEXT("/delta_local_heights/") + FString::FromInt(Index), MakeShared<FJsonValueObject>(Actual),
						NumberArrayValue({-256.0, 255.9921875}), OutAction.TargetRef));
				}
				OutAction.DeltaLocalHeights.Add(static_cast<float>(DeltaValues[Index]));
			}
			return true;
		}

		void CleanupCreatedLandscape(UWorld& World, FLandscapeGeometryAction& Action, FExecutionMutationError& Error)
		{
			ALandscape* Landscape = Action.Landscape.Get();
			const FString NativePath = Landscape ? Landscape->GetPathName() : FString();
			const bool bDestroyed = !Landscape || World.DestroyActor(Landscape);
			Error.MutationStarted = true;
			Error.RollbackAttempted = true;
			Error.RollbackSucceeded = bDestroyed;
			if (bDestroyed)
			{
				Action.Landscape.Reset();
				if (UPackage* Package = Action.Package.Get()) Package->SetDirtyFlag(Action.bPackageWasDirty);
			}
			else
			{
				auto CleanupError = NativeConditionFailure(TEXT("landscape_geometry_create_cleanup_failed"),
					TEXT("The newly created Landscape actor could not be destroyed after the native failure."),
					TEXT("/created_actor_destroyed"), Action.TargetRef);
				CleanupError.Stage = TEXT("rollback");
				CleanupError.SubjectId = NativePath;
				Error.Append(CleanupError);
			}
		}

		bool ApplyCreate(UWorld& World, FLandscapeGeometryAction& Action, FExecutionMutationError& OutError)
		{
			const FString TargetLabel = FPaths::GetBaseFilename(Action.TargetRef);
			for (TActorIterator<ALandscape> It(&World); It; ++It)
				if (It->GetActorLabel() == TargetLabel)
				{
					auto Error = NativeValueFailure(TEXT("landscape_geometry_create_conflict"),
						TEXT("A Landscape actor with the requested label already exists."), TEXT("/actor_label"),
						MakeShared<FJsonValueString>(TargetLabel), MakeShared<FJsonValueString>(TEXT("unused Landscape label")), Action.TargetRef);
					Error.SubjectId = It->GetPathName();
					return Fail(OutError, MoveTemp(Error));
				}
			UPackage* Package = World.PersistentLevel ? World.PersistentLevel->GetOutermost() : World.GetOutermost();
			Action.Package = Package;
			Action.bPackageWasDirty = Package && Package->IsDirty();
			FActorSpawnParameters SpawnParameters;
			SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			SpawnParameters.ObjectFlags |= RF_Transactional;
			ALandscape* Landscape = World.SpawnActor<ALandscape>(Action.Translation, Action.Rotation.Rotator(), SpawnParameters);
			if (!Landscape)
				return Fail(OutError, NativeConditionFailure(TEXT("landscape_geometry_spawn_failed"),
					TEXT("Unreal could not spawn the requested Landscape actor."), TEXT("/spawned_actor_present"), Action.TargetRef));
			Action.Landscape = Landscape;
			Landscape->SetActorScale3D(Action.Scale);
			Landscape->SetActorLabel(TargetLabel);
			UE::UAI::Contract::FDiagnostic OwnershipError;
			if (!UeAgentLevelAuthoring::TryBindManagedActorOwnership(*Landscape, Action.TargetRef, TEXT("landscape"), OwnershipError))
			{
				OutError = MoveTemp(OwnershipError);
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Action.TargetRef;
				CleanupCreatedLandscape(World, Action, OutError);
				return false;
			}
			const int64 Width = static_cast<int64>(Action.MaxX) - Action.MinX + 1;
			const int64 Height = static_cast<int64>(Action.MaxY) - Action.MinY + 1;
			// Equivalent to the positive sample-count limit without overflowing its product.
			if (Width <= 0 || Height <= 0 || Width > MAX_int32 / Height)
			{
				auto Actual = MakeShared<FJsonObject>();
				Actual->SetNumberField(TEXT("width"), Width);
				Actual->SetNumberField(TEXT("height"), Height);
				if (Width > 0 && Height > 0 && Width <= MAX_int64 / Height)
					Actual->SetStringField(TEXT("sample_count"), ::LexToString(Width * Height));
				OutError = NativeValueFailure(TEXT("landscape_geometry_create_size_invalid"),
					TEXT("The Landscape sample dimensions must be positive and the total sample count must fit int32."),
					TEXT("/sample_count"), MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueNumber>(MAX_int32), Action.TargetRef);
				CleanupCreatedLandscape(World, Action, OutError);
				return false;
			}
			TArray<uint16> Heights;
			Heights.Init(LandscapeDataAccess::GetTexHeight(Action.InitialLocalHeight), static_cast<int32>(Width * Height));
			TMap<FGuid, TArray<uint16>> HeightsByLayer;
			HeightsByLayer.Add(FGuid(), MoveTemp(Heights));
			TMap<FGuid, TArray<FLandscapeImportLayerInfo>> MaterialLayersByLayer;
			MaterialLayersByLayer.Add(FGuid(), TArray<FLandscapeImportLayerInfo>());
			const TArrayView<const FLandscapeLayer> EmptyLayers;
			Landscape->Import(FGuid::NewGuid(), Action.MinX, Action.MinY, Action.MaxX, Action.MaxY,
				Action.SectionsPerComponent, Action.QuadsPerSection, HeightsByLayer, TEXT(""),
				MaterialLayersByLayer, ELandscapeImportAlphamapType::Additive, EmptyLayers);
			Landscape->CreateLandscapeInfo();
			Landscape->PostEditChange();
			int32 LiveMinX = 0, LiveMinY = 0, LiveMaxX = 0, LiveMaxY = 0;
			const bool bExtentRead = ReadLiveExtent(*Landscape, LiveMinX, LiveMinY, LiveMaxX, LiveMaxY, OutError, Action.TargetRef);
			if (!bExtentRead || LiveMinX != Action.MinX || LiveMinY != Action.MinY || LiveMaxX != Action.MaxX || LiveMaxY != Action.MaxY)
			{
				OutError = !bExtentRead ? FExecutionMutationError::WrapFailure(TEXT("landscape_geometry_create_readback_failed"), OutError)
					: NativeValueFailure(TEXT("landscape_geometry_create_readback_failed"),
						TEXT("The created Landscape extent differs from the requested sample extent."), TEXT("/sample_extent"),
						RegionValue(LiveMinX, LiveMinY, LiveMaxX, LiveMaxY), RegionValue(Action.MinX, Action.MinY, Action.MaxX, Action.MaxY), Action.TargetRef);
				CleanupCreatedLandscape(World, Action, OutError);
				return false;
			}
			return true;
		}

		bool ApplyHeightDelta(
			FLandscapeGeometryAction& Action,
			FExecutionMutationError& OutError)
		{
			ALandscape* Landscape = Action.Landscape.Get();
			ULandscapeInfo* Info =
				Landscape ? Landscape->GetLandscapeInfo() : nullptr;
			if (!Landscape || !Info)
			{
				return Fail(OutError, NativeConditionFailure(TEXT("landscape_geometry_height_target_lost"),
					!Landscape ? TEXT("The native Landscape actor was lost before its height update.")
						: TEXT("The native LandscapeInfo was lost before its height update."),
					!Landscape ? TEXT("/landscape_present") : TEXT("/landscape_info_present"), Action.TargetRef));
			}
			TArray<uint16> Updated =
				Action.OriginalEncodedHeights;
			for (int32 Index = 0;
				Index < Updated.Num();
				++Index)
			{
				const float CurrentLocalHeight =
					LandscapeDataAccess::GetLocalHeight(
						Updated[Index]);
				Updated[Index] =
					LandscapeDataAccess::GetTexHeight(
						CurrentLocalHeight
							+ Action.DeltaLocalHeights[Index]);
			}
			FLandscapeEditDataInterface Edit(Info);
			Edit.SetHeightData(
				Action.MinX,
				Action.MinY,
				Action.MaxX,
				Action.MaxY,
				Updated.GetData(),
				Action.RowStride,
				true);
			Landscape->PostEditChange();
			Landscape->MarkPackageDirty();
			return true;
		}

		bool FailRollback(FExecutionMutationError& OutError, FExecutionMutationError Error)
		{
			Error.Stage = TEXT("rollback");
			return Fail(OutError, MoveTemp(Error));
		}

		bool RollbackAction(UWorld& World, const FLandscapeGeometryAction& Action, FExecutionMutationError& OutError)
		{
			OutError = {};
			UPackage* Package = Action.Package.Get();
			if (Action.Kind == ELandscapeGeometryActionKind::Create)
			{
				if (ALandscape* Landscape = Action.Landscape.Get())
				{
					UWorld* ActorWorld = Landscape->GetWorld();
					if (ActorWorld != &World)
					{
						auto Error = NativeConditionFailure(TEXT("landscape_geometry_rollback_world_mismatch"),
							TEXT("The created Landscape actor belongs to a different world during rollback."),
							TEXT("/world_matches_requested"), Action.TargetRef);
						Error.ActualEvidence = MakeShared<FJsonValueString>(ActorWorld ? ActorWorld->GetPathName() : FString());
						Error.ExpectedEvidence = MakeShared<FJsonValueString>(World.GetPathName());
						return FailRollback(OutError, MoveTemp(Error));
					}
					const FString NativePath = Landscape->GetPathName();
					if (!World.DestroyActor(Landscape))
					{
						auto Error = NativeConditionFailure(TEXT("landscape_geometry_rollback_destroy_failed"),
							TEXT("Unreal could not destroy the created Landscape actor during rollback."),
							TEXT("/created_actor_destroyed"), Action.TargetRef);
						Error.SubjectId = NativePath;
						return FailRollback(OutError, MoveTemp(Error));
					}
				}
				if (Package) Package->SetDirtyFlag(Action.bPackageWasDirty);
				return true;
			}
			ALandscape* Landscape = Action.Landscape.Get();
			ULandscapeInfo* Info = Landscape ? Landscape->GetLandscapeInfo() : nullptr;
			if (!Landscape)
				return FailRollback(OutError, NativeConditionFailure(TEXT("landscape_geometry_rollback_actor_missing"),
					TEXT("The Landscape actor is no longer available for height rollback."), TEXT("/landscape_present"), Action.TargetRef));
			UWorld* ActorWorld = Landscape->GetWorld();
			if (ActorWorld != &World)
			{
				auto Error = NativeConditionFailure(TEXT("landscape_geometry_rollback_world_mismatch"),
					TEXT("The Landscape actor belongs to a different world during height rollback."),
					TEXT("/world_matches_requested"), Action.TargetRef);
				Error.ActualEvidence = MakeShared<FJsonValueString>(ActorWorld ? ActorWorld->GetPathName() : FString());
				Error.ExpectedEvidence = MakeShared<FJsonValueString>(World.GetPathName());
				return FailRollback(OutError, MoveTemp(Error));
			}
			if (!Info)
				return FailRollback(OutError, NativeConditionFailure(TEXT("landscape_geometry_rollback_info_missing"),
					TEXT("The native LandscapeInfo is no longer available for height rollback."), TEXT("/landscape_info_present"), Action.TargetRef));
			if (Action.OriginalEncodedHeights.IsEmpty())
				return FailRollback(OutError, NativeValueFailure(TEXT("landscape_geometry_rollback_heights_missing"),
					TEXT("No original height samples were retained for rollback."), TEXT("/original_heights/count"),
					MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("at least one sample")), Action.TargetRef));
			FLandscapeEditDataInterface Edit(Info);
			Edit.SetHeightData(Action.MinX, Action.MinY, Action.MaxX, Action.MaxY,
				Action.OriginalEncodedHeights.GetData(), Action.RowStride, true);
			Landscape->PostEditChange();
			if (Package) Package->SetDirtyFlag(Action.bPackageWasDirty);
			return true;
		}

		bool RollbackAll(UWorld& World, const TArray<FLandscapeGeometryAction>& Actions, FExecutionMutationError& OutError)
		{
			OutError = {};
			bool bConfirmed = true;
			for (int32 Index = Actions.Num() - 1; Index >= 0; --Index)
			{
				FExecutionMutationError ActionError;
				if (!RollbackAction(World, Actions[Index], ActionError))
				{
					bConfirmed = false;
					ActionError.OperationIndex = Index;
					OutError.Append(ActionError);
				}
			}
			return bConfirmed;
		}

		class FLandscapeGeometryEffectNativeHook final
			: public IUeAgentLandscapeGeometryEffectNativeHook
		{
		public:
			explicit FLandscapeGeometryEffectNativeHook(
				TSharedRef<
					IUeAgentLandscapeGeometryEffectReferenceResolver,
					ESPMode::ThreadSafe> InResolver)
				: Resolver(MoveTemp(InResolver))
			{
			}

			bool TryApply(
				UWorld& World,
				const FExecutionMutationRequest& Request,
				const FLockFenceLease& Lease,
				const TArray<FUeAgentGenericEffectBinding>& Bindings,
				FUeAgentResourceTracker& Resources,
				FUeAgentGenericEffectApplyResult& OutResult,
				FExecutionMutationError& OutError) override
			{
				OutResult = FUeAgentGenericEffectApplyResult::NoMutation();
				OutError = FExecutionMutationError();
				if (!IsInGameThread())
				{
					return Fail(OutError, NativeConditionFailure(TEXT("landscape_geometry_game_thread_required"),
						TEXT("Native Landscape mutation must run on GameThread."), TEXT("/game_thread")));
				}
				if (!Request.IsValid())
					return Fail(OutError, FExecutionMutationError::WrapFailure(TEXT("landscape_geometry_exact_world_context_mismatch"), Request.GetValidationDiagnostic()));
				if (!Lease.IsValid())
					return Fail(OutError, FExecutionMutationError::WrapFailure(TEXT("landscape_geometry_exact_world_context_mismatch"), Lease.GetValidationDiagnostic()));
				if (Request.GetExecutionContext()
							.GetWorldIdentity()
						!= World.GetPathName())
				{
					auto Error = NativeConditionFailure(TEXT("landscape_geometry_exact_world_context_mismatch"),
						TEXT("The selected native world differs from the execution context world."), TEXT("/world_matches_execution_context"));
					Error.ActualEvidence = MakeShared<FJsonValueString>(World.GetPathName());
					Error.ExpectedEvidence = MakeShared<FJsonValueString>(Request.GetExecutionContext().GetWorldIdentity());
					return Fail(OutError, MoveTemp(Error));
				}
				const TArray<FPlannedChange>& Changes =
					Request.GetPlan()
						->GetChangeSet()
						.GetChanges();
				if (Changes.IsEmpty())
					return Fail(OutError, NativeValueFailure(TEXT("landscape_geometry_change_binding_mismatch"),
						TEXT("Native Landscape mutation requires at least one planned change."), TEXT("/changes/count"),
						MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT(">= 1"))));
				if (Bindings.Num() != Changes.Num())
				{
					return Fail(OutError, NativeValueFailure(TEXT("landscape_geometry_change_binding_mismatch"),
						TEXT("The native Landscape binding count differs from the planned change count."), TEXT("/bindings/count"),
						MakeShared<FJsonValueNumber>(Bindings.Num()), MakeShared<FJsonValueNumber>(Changes.Num())));
				}

				TArray<FLandscapeGeometryAction> Actions;
				Actions.Reserve(Changes.Num());
				for (const FPlannedChange& Change : Changes)
				{
					FLandscapeGeometryAction Action;
					const FString& TargetKind =
						Change.GetIdentity().GetTargetKind();
					const bool bPrepared =
						TargetKind
							== TEXT("landscape.geometry.create")
						? PrepareCreate(
							Change,
							Action,
							OutError)
						: TargetKind
								== TEXT(
									"landscape.geometry.height_delta")
							? PrepareHeightDelta(
								World,
								*Resolver,
								Change,
								Action,
								OutError)
							: Fail(OutError, NativeValueFailure(TEXT("landscape_geometry_target_kind_unsupported"),
								TEXT("The native Landscape hook does not support this target kind."), TEXT("/target_kind"),
								MakeShared<FJsonValueString>(TargetKind), MakeShared<FJsonValueString>(TEXT("landscape.geometry.create or landscape.geometry.height_delta"))));
					if (!bPrepared)
					{
						if (OutError.TargetPath.IsEmpty())
						{
							const auto* Binding = Bindings.FindByPredicate([&Change](const auto& Item)
							{
								return Item.GetCandidateBinding().GetSummary().GetIdentity().GetCanonicalKey() == Change.GetIdentity().GetCanonicalKey();
							});
							OutError.TargetPath = !Action.TargetRef.IsEmpty() ? Action.TargetRef : Binding ? Binding->GetCandidateBinding().GetPublicRef() : FString();
						}
						if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Change.GetIdentity().GetCanonicalKey();
						if (!OutError.OperationIndex.IsSet()) OutError.OperationIndex = Actions.Num();
						return false;
					}
					Actions.Add(MoveTemp(Action));
				}

				TArray<FLandscapeGeometryAction> AppliedActions;
				AppliedActions.Reserve(Actions.Num());
				TArray<FString> MutatedKeys;
				for (FLandscapeGeometryAction& Action : Actions)
				{
					OutResult = FUeAgentGenericEffectApplyResult();
					const bool bApplied =
						Action.Kind
							== ELandscapeGeometryActionKind::Create
						? ApplyCreate(World, Action, OutError)
						: ApplyHeightDelta(Action, OutError);
					if (!bApplied)
					{
						FExecutionMutationError RollbackError;
						const bool bRollbackConfirmed = RollbackAll(World, AppliedActions, RollbackError);
						OutError.Append(RollbackError);
						if (!AppliedActions.IsEmpty())
						{
							OutError.MutationStarted = true;
							OutError.RollbackAttempted = true;
							if (!bRollbackConfirmed) OutError.RollbackSucceeded = false;
						}
						return false;
					}
					MutatedKeys.Add(Action.CanonicalKey);
					AppliedActions.Add(MoveTemp(Action));
				}

				const FString Token = FString::Printf(
					TEXT("landscape-geometry:%s:%lld"),
					*Request.GetOperationId(),
					Lease.GetFencingToken());
				{
					FScopeLock Lock(&StateMutex);
					RollbackByToken.Add(Token, AppliedActions);
					TokenByOperation.Add(
						Request.GetOperationId(),
						Token);
				}
				OutResult = FUeAgentGenericEffectApplyResult(
					Lease.GetFencingToken(),
					MoveTemp(MutatedKeys),
					{TEXT("landscape.authoring.memory")},
					true,
					Token,
					true);
				return OutResult.IsValid()
					|| Fail(OutError, FExecutionMutationError::WrapFailure(TEXT("landscape_geometry_apply_result_invalid"), OutResult.GetValidationDiagnostic()));
			}

			bool TryRollback(
				UWorld& World,
				const FExecutionMutationRequest& Request,
				const FUeAgentGenericEffectApplyResult& Applied,
				FUeAgentResourceTracker& Resources,
				bool& bOutConfirmed,
				FExecutionMutationError& OutError) override
			{
				bOutConfirmed = false;
				OutError = FExecutionMutationError();
				if (!IsInGameThread())
					return FailRollback(OutError, NativeConditionFailure(TEXT("landscape_geometry_rollback_request_invalid"),
						TEXT("Native Landscape rollback must run on GameThread."), TEXT("/game_thread")));
				if (!Applied.IsValid())
					return Fail(OutError, FExecutionMutationError::WrapFailure(TEXT("landscape_geometry_rollback_request_invalid"), Applied.GetValidationDiagnostic()));
				if (Request.GetExecutionContext()
							.GetWorldIdentity()
						!= World.GetPathName())
				{
					auto Error = NativeConditionFailure(TEXT("landscape_geometry_rollback_request_invalid"),
						TEXT("The selected rollback world differs from the execution context world."), TEXT("/world_matches_execution_context"));
					Error.ActualEvidence = MakeShared<FJsonValueString>(World.GetPathName());
					Error.ExpectedEvidence = MakeShared<FJsonValueString>(Request.GetExecutionContext().GetWorldIdentity());
					return FailRollback(OutError, MoveTemp(Error));
				}
				TArray<FLandscapeGeometryAction> Actions;
				{
					FScopeLock Lock(&StateMutex);
					const TArray<FLandscapeGeometryAction>* Found =
						RollbackByToken.Find(
							Applied.GetRollbackToken());
					if (!Found)
					{
						auto Error = NativeConditionFailure(TEXT("landscape_geometry_rollback_token_unknown"),
							TEXT("No retained Landscape rollback actions match the supplied token."), TEXT("/rollback_token_known"));
						Error.SubjectId = Applied.GetRollbackToken();
						return FailRollback(OutError, MoveTemp(Error));
					}
					Actions = *Found;
				}
				bOutConfirmed = RollbackAll(World, Actions, OutError);
				if (!bOutConfirmed)
				{
					OutError = FExecutionMutationError::WrapFailure(TEXT("landscape_geometry_rollback_unconfirmed"), OutError);
					OutError.RollbackAttempted = true;
					OutError.RollbackSucceeded = false;
					return false;
				}
				{
					FScopeLock Lock(&StateMutex);
					RollbackByToken.Remove(
						Applied.GetRollbackToken());
					TokenByOperation.Remove(
						Request.GetOperationId());
				}
				return true;
			}

			void ReleaseVerifiedOperation(
				const FString& OperationId) override
			{
				FScopeLock Lock(&StateMutex);
				if (const FString* Token =
					TokenByOperation.Find(OperationId))
				{
					RollbackByToken.Remove(*Token);
				}
				TokenByOperation.Remove(OperationId);
			}

			bool TryWaitForSettle(
				const FBarrierRequest& Request,
				FUeAgentResourceTracker& Resources,
				FBarrierResult& OutResult,
				FExecutionMutationError& OutError) override
			{
				OutError = FExecutionMutationError();
				if (!Request.IsValid())
				{
					auto Error = NativeConditionFailure(TEXT("landscape_geometry_barrier_request_invalid"),
						TEXT("The Landscape barrier subject generation must be non-negative."), TEXT("/subject_generation_non_negative"));
					Error.ActualEvidence = MakeShared<FJsonValueString>(::LexToString(Request.GetSubjectGeneration()));
					Error.Stage = TEXT("settling");
					return Fail(OutError, MoveTemp(Error));
				}
				OutResult = FBarrierResult(
					Request.GetSubjectGeneration(),
					Request.GetRequiredSignals(),
					FString::Printf(
						TEXT("landscape-geometry:%lld"),
						Request.GetSubjectGeneration()),
					true);
				return OutResult.IsValid()
					|| Fail(OutError, FExecutionMutationError::WrapFailure(TEXT("landscape_geometry_barrier_result_invalid"), OutResult.GetValidationDiagnostic()));
			}

		private:
			TSharedRef<
				IUeAgentLandscapeGeometryEffectReferenceResolver,
				ESPMode::ThreadSafe> Resolver;
			FCriticalSection StateMutex;
			TMap<
				FString,
				TArray<FLandscapeGeometryAction>> RollbackByToken;
			TMap<FString, FString> TokenByOperation;
		};
	}

	bool FUeAgentLandscapeGeometryEffectNativeHookFactory::TryCreate(
		TSharedRef<
			IUeAgentLandscapeGeometryEffectReferenceResolver,
			ESPMode::ThreadSafe> Resolver,
		TSharedPtr<
			IUeAgentLandscapeGeometryEffectNativeHook>& OutHook,
		FExecutionMutationError& OutError)
	{
		OutHook =
			MakeShared<FLandscapeGeometryEffectNativeHook>(
				MoveTemp(Resolver));
		OutError = FExecutionMutationError();
		return OutHook.IsValid();
	}
}

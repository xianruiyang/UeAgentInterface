#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialNative.h"
#include "Utilities/EditorResources/UeAgentEditorResourcePersistence.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Compilation/MovieSceneCompiledDataManager.h"
#include "ControlRig.h"
#include "ControlRigObjectBinding.h"
#include "ControlRigSequencerEditorLibrary.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "FileHelpers.h"
#include "Exporters/AnimSeqExportOption.h"
#include "HAL/PlatformTime.h"
#include "LevelSequence.h"
#include "Misc/Crc.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieSceneBindingProxy.h"
#include "MovieSceneTimeHelpers.h"
#include "Rigs/FKControlRig.h"
#include "Sequencer/MovieSceneControlRigParameterSection.h"
#include "SequencerTools.h"
#include "Sequencer/MovieSceneControlRigParameterTrack.h"
#include "Tracks/MovieSceneSkeletalAnimationTrack.h"
#include "Units/Execution/RigUnit_InverseExecution.h"

namespace UE::UAI::DomainAdapters::RigSimulationSpatial
{
	using namespace BindingGateway::Operation;
	using namespace ContextResource;
	using namespace Discovery;
	using namespace StateSemantic;
	using namespace Verification;

	namespace
	{
		using UE::UAI::Contract::FDiagnostic;

		bool NativeFailure(FDiagnostic& OutError, const FString& Code, const FString& Reason,
			const FString& Path, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
			const FString& Target = FString())
		{
			OutError = FDiagnostic::Failure(Code, Reason, TEXT("native_execution"));
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = Path;
			OutError.Source.JsonPath = Path;
			OutError.TargetPath = Target;
			OutError.ActualValuePresent = Actual.IsValid();
			OutError.ActualValue = MoveTemp(Actual);
			OutError.ExpectedValuePresent = Expected.IsValid();
			OutError.ExpectedValue = MoveTemp(Expected);
			return false;
		}

		bool InputFailure(FDiagnostic& OutError, const FString& Code, const FString& Reason,
			const FString& Path, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
			const FString& Target = FString())
		{
			NativeFailure(OutError, Code, Reason, Path, MoveTemp(Actual), MoveTemp(Expected), Target);
			OutError.SourceKind = TEXT("command_input");
			return false;
		}

		bool MissingNative(FDiagnostic& OutError, const FString& Code, const FString& Reason,
			const FString& Path, const FString& Target)
		{
			return NativeFailure(OutError, Code, Reason, Path,
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Target);
		}

		bool UnsupportedCapability(FDiagnostic& OutError, const FString& Capability)
		{
			return NativeFailure(OutError, TEXT("rig_simulation_spatial_family_port_unavailable"),
				TEXT("This native port does not implement the requested capability."), TEXT("/capability_id"),
				MakeShared<FJsonValueString>(Capability), MakeShared<FJsonValueString>(
					TEXT("rig.skeleton.preview_mesh.bind, rig.skeleton.socket.update, rig.control.bake.animation, or rig.control.bake.controls")));
		}

		void AppendDifference(FDiagnostic& OutDifference, FDiagnostic Difference)
		{
			Difference.Stage = TEXT("verification");
			if (OutDifference.IsEmpty()) OutDifference = MoveTemp(Difference);
			else OutDifference.Diagnostics.Add(MoveTemp(Difference));
		}

		bool MatchFact(bool bMatches, FDiagnostic& OutDifference, const FString& Reason,
			const FString& Path, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
			const FString& Target)
		{
			if (bMatches) return true;
			FDiagnostic Difference;
			NativeFailure(Difference, TEXT("rig_simulation_spatial_native_state_mismatch"),
				Reason, Path, MoveTemp(Actual), MoveTemp(Expected), Target);
			AppendDifference(OutDifference, MoveTemp(Difference));
			return false;
		}

		void NativeContext(FDiagnostic& Error, const FString& Path = FString(), bool bCanonicalValue = false)
		{
			Error.SourceKind = TEXT("native_result");
			if (!Path.IsEmpty() || bCanonicalValue)
			{
				auto Locate = [&Path, bCanonicalValue](FString Pointer)
				{
					// Canonical constructors locate their value parameter at /value.
					if (bCanonicalValue && (Pointer == TEXT("/value") || Pointer.StartsWith(TEXT("/value/"))))
						Pointer.RightChopInline(6);
					if (Pointer.IsEmpty() || Pointer == TEXT("/")) return Path;
					if (!Pointer.StartsWith(TEXT("/")))
						Pointer = TEXT("/") + Pointer.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
					return Path + Pointer;
				};
				if (Error.Source.InternalJsonPath.IsEmpty()) Error.Source.InternalJsonPath = Error.Source.JsonPath;
				Error.Source.JsonPath = Locate(Error.Source.JsonPath);
				Error.PropertyPath = Error.PropertyPath.IsEmpty() ? Error.Source.JsonPath : Locate(Error.PropertyPath);
				if (!Error.SemanticPointer.IsEmpty()) Error.SemanticPointer = Locate(Error.SemanticPointer);
			}
			for (auto& Child : Error.Diagnostics) NativeContext(Child, Path, bCanonicalValue);
			if (Error.PublicCause)
			{
				auto Cause = MakeShared<FDiagnostic>(*Error.PublicCause);
				NativeContext(*Cause, Path, bCanonicalValue);
				Error.PublicCause = MoveTemp(Cause);
			}
		}

		bool NativeFailureAt(FDiagnostic& OutError, FDiagnostic Error, const FString& Path, const FString& Target = FString())
		{
			NativeContext(Error, Path);
			if (Error.TargetPath.IsEmpty()) Error.TargetPath = Target;
			OutError = MoveTemp(Error);
			return false;
		}

		bool TrySaveProjectAsset(
			UObject& Object,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			UPackage* Package = Object.GetOutermost();
			if (!UE::UAI::EditorResources::IsPersistentPackage(Package)
				|| !Package->IsDirty())
			{
				return true;
			}
			if (!UE::UAI::EditorResources::SavePackages({Package}, false, OutError))
			{
				OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(
					TEXT("rig_simulation_spatial_persistence_failed"), OutError, TEXT("execution"));
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Object.GetPathName();
				return false;
			}
			return true;
		}

		bool IsImplementedCapability(const FString& CapabilityId)
		{
			return CapabilityId == TEXT("rig.skeleton.preview_mesh.bind")
				|| CapabilityId == TEXT("rig.control.bake.context")
				|| CapabilityId == TEXT("rig.skeleton.socket.update")
				|| CapabilityId == TEXT("rig.control.bake.animation")
				|| CapabilityId == TEXT("rig.control.bake.controls");
		}

		const FUeAgentRigSimulationSpatialRequest* ProductionRequest(
			const FUeAgentRigSimulationSpatialTypedRequestRef& Request,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			if (Request->GetTypeId().IsEmpty() || Request->GetFamilyId().IsEmpty()
				|| Request->GetRequestKindId().IsEmpty())
			{
				NativeFailure(OutError, TEXT("rig_simulation_spatial_native_typed_request_invalid"),
					TEXT("The typed request is missing required routing metadata."),
					Request->GetTypeId().IsEmpty() ? TEXT("/type_id")
						: Request->GetFamilyId().IsEmpty() ? TEXT("/family_id") : TEXT("/request_kind"),
					MakeShared<FJsonValueString>(FString()), MakeShared<FJsonValueString>(TEXT("a nonempty identifier")));
				return nullptr;
			}
			return static_cast<const FUeAgentRigSimulationSpatialRequest*>(&Request.Get());
		}

		TOptional<FUeAgentCandidateBinding> FirstTarget(
			const TArray<FUeAgentOperationTargetBinding>& Targets)
		{
			return Targets.IsEmpty() ? TOptional<FUeAgentCandidateBinding>()
				: TOptional<FUeAgentCandidateBinding>(Targets[0].GetBinding());
		}

		bool TryRate(
			const FRigSimulationSpatialRate& Rate,
			FFrameRate& OutRate,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			auto Invalid = [&OutError](const FString& Field, const FString& Actual, const FString& Expected)
			{
				return InputFailure(OutError, TEXT("rig_simulation_spatial_native_rate_invalid"),
					TEXT("The sample-rate component is outside the supported integer domain."),
					TEXT("/sample_rate/") + Field, MakeShared<FJsonValueString>(Actual), MakeShared<FJsonValueString>(Expected));
			};
			int64 WideNumerator = 0;
			int64 WideDenominator = 0;
			if (!LexTryParseString(WideNumerator, *Rate.Numerator))
				return Invalid(TEXT("numerator"), Rate.Numerator, TEXT("a positive int64 integer"));
			if (!LexTryParseString(WideDenominator, *Rate.Denominator))
				return Invalid(TEXT("denominator"), Rate.Denominator, TEXT("a positive int64 integer"));
			if (WideNumerator <= 0) return Invalid(TEXT("numerator"), Rate.Numerator, TEXT("a positive integer"));
			if (WideDenominator <= 0) return Invalid(TEXT("denominator"), Rate.Denominator, TEXT("a positive integer"));
			int32 Numerator = 0;
			int32 Denominator = 0;
			if (!LexTryParseString(Numerator, *Rate.Numerator))
				return Invalid(TEXT("numerator"), Rate.Numerator, TEXT("a positive int32 integer"));
			if (!LexTryParseString(Denominator, *Rate.Denominator))
				return Invalid(TEXT("denominator"), Rate.Denominator, TEXT("a positive int32 integer"));
			if (Numerator <= 0) return Invalid(TEXT("numerator"), Rate.Numerator, TEXT("a positive int32 integer"));
			if (Denominator <= 0) return Invalid(TEXT("denominator"), Rate.Denominator, TEXT("a positive int32 integer"));
			OutRate = FFrameRate(Numerator, Denominator);
			return true;
		}

		bool TryRangeFrames(
			const FUeAgentTemporalRange& Range,
			int32& OutLower,
			int32& OutUpper,
			FFrameRate& OutDisplayRate,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			const TOptional<FUeAgentTemporalPoint>& Lower =
				Range.GetLowerBound().GetPoint();
			const TOptional<FUeAgentTemporalPoint>& Upper =
				Range.GetUpperBound().GetPoint();
			int64 LowerValue = 0;
			int64 UpperValue = 0;
			int32 Numerator = 0;
			int32 Denominator = 0;
			if (!Range.IsValid())
				return NativeFailureAt(OutError, Range.GetValidationDiagnostic(), TEXT("/converted_range"));
			if (!Lower.IsSet() || !Upper.IsSet())
				return MissingNative(OutError, TEXT("rig_simulation_spatial_native_authored_range_invalid"),
					TEXT("Native baking requires both range bounds to contain time points."),
					!Lower.IsSet() ? TEXT("/converted_range/lower") : TEXT("/converted_range/upper"), FString());
			auto Invalid = [&OutError](const FString& Path, const FString& Actual, const FString& Expected, const FString& Reason)
			{
				return NativeFailure(OutError, TEXT("rig_simulation_spatial_native_authored_range_invalid"),
					Reason, TEXT("/converted_range/") + Path, MakeShared<FJsonValueString>(Actual), MakeShared<FJsonValueString>(Expected));
			};
			if (!LexTryParseString(LowerValue, *Lower->GetTick()))
				return Invalid(TEXT("lower/tick"), Lower->GetTick(), TEXT("an int64 integer"), TEXT("The lower native tick cannot be parsed."));
			if (!LexTryParseString(UpperValue, *Upper->GetTick()))
				return Invalid(TEXT("upper/tick"), Upper->GetTick(), TEXT("an int64 integer"), TEXT("The upper native tick cannot be parsed."));
			if (!LexTryParseString(Numerator, *Lower->GetRateNumerator()))
				return Invalid(TEXT("lower/rate/numerator"), Lower->GetRateNumerator(), TEXT("an int32 integer"), TEXT("The native rate numerator cannot be parsed."));
			if (!LexTryParseString(Denominator, *Lower->GetRateDenominator()))
				return Invalid(TEXT("lower/rate/denominator"), Lower->GetRateDenominator(), TEXT("an int32 integer"), TEXT("The native rate denominator cannot be parsed."));
			if (LowerValue < MIN_int32 || LowerValue > MAX_int32)
				return Invalid(TEXT("lower/tick"), Lower->GetTick(), TEXT("an int32 frame number"), TEXT("The lower native frame exceeds the supported range."));
			if (UpperValue < MIN_int32 || UpperValue > MAX_int32)
				return Invalid(TEXT("upper/tick"), Upper->GetTick(), TEXT("an int32 frame number"), TEXT("The upper native frame exceeds the supported range."));
			if (UpperValue < LowerValue)
				return Invalid(TEXT("upper/tick"), Upper->GetTick(), TEXT(">= ") + Lower->GetTick(), TEXT("The upper native frame precedes the lower frame."));
			if (Numerator <= 0)
				return Invalid(TEXT("lower/rate/numerator"), Lower->GetRateNumerator(), TEXT("a positive integer"), TEXT("The native rate numerator is not positive."));
			if (Denominator <= 0)
				return Invalid(TEXT("lower/rate/denominator"), Lower->GetRateDenominator(), TEXT("a positive integer"), TEXT("The native rate denominator is not positive."));
			if (Upper->GetRateNumerator() != Lower->GetRateNumerator())
				return Invalid(TEXT("upper/rate/numerator"), Upper->GetRateNumerator(), Lower->GetRateNumerator(), TEXT("The native range bounds use different rate numerators."));
			if (Upper->GetRateDenominator() != Lower->GetRateDenominator())
				return Invalid(TEXT("upper/rate/denominator"), Upper->GetRateDenominator(), Lower->GetRateDenominator(), TEXT("The native range bounds use different rate denominators."));
			OutLower = static_cast<int32>(LowerValue);
			OutUpper = static_cast<int32>(UpperValue);
			OutDisplayRate = FFrameRate(Numerator, Denominator);
			return true;
		}

		bool TryNativeTransform(
			const FUeAgentSpatialValue& Value,
			FTransform& OutTransform,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			const TArray<FString>& Components = Value.GetComponents();
			if (!Value.IsValid())
				return NativeFailureAt(OutError, Value.GetValidationDiagnostic(), TEXT("/converted_transform"));
			if (Value.GetQuantityKind() != EUeAgentSpatialQuantityKind::Transform)
				return NativeFailure(OutError, TEXT("rig_simulation_spatial_native_transform_invalid"),
					TEXT("The converted spatial value is not a transform."), TEXT("/converted_transform/quantity_kind"),
					MakeShared<FJsonValueString>(Value.GetQuantityKind() == EUeAgentSpatialQuantityKind::Point ? TEXT("point")
						: Value.GetQuantityKind() == EUeAgentSpatialQuantityKind::Direction ? TEXT("direction") : TEXT("normal")),
					MakeShared<FJsonValueString>(TEXT("transform")));
			if (Components.Num() != 16)
				return NativeFailure(OutError, TEXT("rig_simulation_spatial_native_transform_invalid"),
					TEXT("The converted transform requires sixteen matrix components."), TEXT("/converted_transform/components"),
					MakeShared<FJsonValueNumber>(Components.Num()), MakeShared<FJsonValueNumber>(16));
			FMatrix Matrix = FMatrix::Identity;
			for (int32 Row = 0; Row < 4; ++Row)
			{
				for (int32 Column = 0; Column < 4; ++Column)
				{
					double Component = 0.0;
					if (!LexTryParseString(Component,
							*Components[Row * 4 + Column])
						|| !FMath::IsFinite(Component))
					{
						return NativeFailure(OutError,
							TEXT("rig_simulation_spatial_native_transform_numeric_invalid"),
							TEXT("The converted transform component is not a finite number."),
							TEXT("/converted_transform/components/") + FString::FromInt(Row * 4 + Column),
							MakeShared<FJsonValueString>(Components[Row * 4 + Column]), MakeShared<FJsonValueString>(TEXT("a finite number")));
					}
					// C4 is a column-vector homogeneous matrix.  UE's FMatrix is
					// row-vector based, so the explicit representation is transposed.
					Matrix.M[Column][Row] = Component;
				}
			}
			OutTransform = FTransform(Matrix);
			OutTransform.NormalizeRotation();
			if (OutTransform.IsValid()) return true;
			const FQuat Rotation = OutTransform.GetRotation();
			const FVector Translation = OutTransform.GetTranslation();
			const FVector Scale = OutTransform.GetScale3D();
			const double NativeComponents[] = {Rotation.X, Rotation.Y, Rotation.Z, Rotation.W,
				Translation.X, Translation.Y, Translation.Z, Scale.X, Scale.Y, Scale.Z};
			const TCHAR* Paths[] = {TEXT("/rotation/x"), TEXT("/rotation/y"), TEXT("/rotation/z"), TEXT("/rotation/w"),
				TEXT("/translation/x"), TEXT("/translation/y"), TEXT("/translation/z"), TEXT("/scale/x"), TEXT("/scale/y"), TEXT("/scale/z")};
			for (int32 Index = 0; Index < UE_ARRAY_COUNT(NativeComponents); ++Index)
				if (!FMath::IsFinite(NativeComponents[Index]))
					return NativeFailure(OutError, TEXT("rig_simulation_spatial_native_transform_invalid"),
						TEXT("The native transform contains a non-finite component."), TEXT("/converted_transform") + FString(Paths[Index]),
						MakeShared<FJsonValueString>(FString::Printf(TEXT("%.17g"), NativeComponents[Index])),
						MakeShared<FJsonValueString>(TEXT("a finite number")));
			return NativeFailure(OutError, TEXT("rig_simulation_spatial_native_transform_invalid"),
				TEXT("The native transform quaternion is not normalized."), TEXT("/converted_transform/rotation"),
				MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
					MakeShared<FJsonValueNumber>(Rotation.X), MakeShared<FJsonValueNumber>(Rotation.Y),
					MakeShared<FJsonValueNumber>(Rotation.Z), MakeShared<FJsonValueNumber>(Rotation.W)}),
				MakeShared<FJsonValueString>(TEXT("a normalized quaternion")));
		}

		FString TransformText(const FTransform& Transform)
		{
			const FVector Translation = Transform.GetTranslation();
			const FQuat Rotation = Transform.GetRotation();
			const FVector Scale = Transform.GetScale3D();
			return FString::Printf(
				TEXT("t=%.9g,%.9g,%.9g|q=%.9g,%.9g,%.9g,%.9g|s=%.9g,%.9g,%.9g"),
				Translation.X, Translation.Y, Translation.Z,
				Rotation.X, Rotation.Y, Rotation.Z, Rotation.W,
				Scale.X, Scale.Y, Scale.Z);
		}

		int32 CountControlKeys(const ULevelSequence& Sequence)
		{
			int32 Result = 0;
			const UMovieScene* Scene = Sequence.GetMovieScene();
			if (!Scene) return Result;
			for (const FMovieSceneBinding& Binding : Scene->GetBindings())
			{
				for (const UMovieSceneTrack* Track : Binding.GetTracks())
				{
					const UMovieSceneControlRigParameterTrack* RigTrack =
						Cast<UMovieSceneControlRigParameterTrack>(Track);
					if (!RigTrack) continue;
					for (const UMovieSceneSection* RawSection : RigTrack->GetAllSections())
					{
						const UMovieSceneControlRigParameterSection* Section =
							Cast<UMovieSceneControlRigParameterSection>(RawSection);
						if (!Section) continue;
						for (const FTransformParameterNameAndCurves& Curves
							: Section->GetTransformParameterNamesAndCurves())
						{
							for (int32 Axis = 0; Axis < 3; ++Axis)
							{
								Result += Curves.Translation[Axis]
									.GetData().GetTimes().Num();
								Result += Curves.Rotation[Axis]
									.GetData().GetTimes().Num();
								Result += Curves.Scale[Axis]
									.GetData().GetTimes().Num();
							}
						}
					}
				}
			}
			return Result;
		}

		bool TryStringValue(
			const FString& Text,
			const FUeAgentOperationContext& Context,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentRigSimulationSpatialExecutionError& OutError,
			const FString& Path)
		{
			FUeAgentStateSemanticError Error;
			if (FUeAgentCanonicalValue::TryString(Text, Context.GetResourceTracker(), OutValue, Error)) return true;
			NativeContext(Error, Path, true);
			OutError = FDiagnostic::WrapFailure(TEXT("rig_simulation_spatial_native_string_failed"), Error, TEXT("native_execution"));
			return false;
		}

		bool TrySnapshot(
			const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
			EUeAgentRigSimulationSpatialReadPhase Phase,
			const FString& PublicRef,
			const FString& State,
			const FUeAgentOperationContext& Context,
			TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
			FUeAgentRigSimulationSpatialExecutionError& OutError,
			const TSharedPtr<FJsonObject>& BakeContext = nullptr)
		{
			FUeAgentCanonicalValuePtr RefValue;
			FUeAgentCanonicalValuePtr CapabilityValue;
			FUeAgentCanonicalValuePtr StateValue;
			FUeAgentCanonicalValuePtr ObjectValue;
			FUeAgentStateSemanticError Error;
			if (!TryStringValue(PublicRef, Context, RefValue, OutError, TEXT("/public_ref"))
				|| !TryStringValue(Contract.GetCapabilityId(), Context,
					CapabilityValue, OutError, TEXT("/capability_id"))
				|| !TryStringValue(State, Context, StateValue, OutError, TEXT("/observed_state"))) return false;
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields = {
				{TEXT("public_ref"), RefValue.ToSharedRef()},
				{TEXT("capability_id"), CapabilityValue.ToSharedRef()},
				{TEXT("observed_state"), StateValue.ToSharedRef()}};
			if (BakeContext)
			{
				if (!CanonicalJson::TryFromJsonValue(MakeShared<FJsonValueObject>(BakeContext),
					Context.GetResourceTracker(), ObjectValue, OutError)) return false;
			}
			else if (!FUeAgentCanonicalValue::TryObject(Fields,
				Context.GetResourceTracker(), ObjectValue, Error))
			{
				if (OutError.IsValid()) return false;
				NativeContext(Error, FString(), true);
				OutError = FDiagnostic::WrapFailure(TEXT("rig_simulation_spatial_native_object_failed"), Error, TEXT("native_execution"));
				return false;
			}
			const uint64 Upper = FCrc::StrCrc32(*State);
			const uint64 Lower = FCrc::StrCrc32(*(Contract.GetReadbackId() + State));
			const FUeAgentStateRevision Revision(Contract.GetReadbackId(),
				TEXT("native-observation-v1"),
				static_cast<int64>(((Upper << 32) | Lower) & MAX_int64));
			const FUeAgentStateEntry Entry(
				FUeAgentCanonicalIdentity(
					TEXT("rig-simulation-spatial:") + Contract.GetCapabilityId(),
					TEXT("rig_simulation_spatial_target"), PublicRef, PublicRef,
					BakeContext ? TEXT("bake_context") : TEXT("result"), TEXT("native-observation-v1")),
				FUeAgentCanonicalType(Contract.GetTypedResultId(),
					EUeAgentValueKind::Object, false, TEXT("object")),
				ObjectValue.ToSharedRef(),
				FUeAgentDefaultValue(EUeAgentDefaultSourceKind::DomainNative,
					Contract.GetReadbackId(), TEXT("independent-native-readback"),
					ObjectValue.ToSharedRef()),
				FUeAgentStateDetails(TEXT("result"),
					TEXT("independent RigSimulationSpatial native observation"),
					{TEXT("RigSimulationSpatial"), Contract.GetFamilyId()}, false,
					TEXT("available"), {}), Revision);
			if (!FUeAgentCanonicalSnapshot::TryCreate(
					FString::Printf(TEXT("rig-simulation-spatial:%s:%d:%s"),
						*Contract.GetCapabilityId(), static_cast<int32>(Phase),
						*Context.GetRequestId()),
					Context.GetExecutionContext().GetStatePlane(), Revision,
					FUeAgentTemporalPoint(TEXT("editor-native"),
						FString::Printf(TEXT("%llu"), FPlatformTime::Cycles64())),
					FUeAgentStateProvenance(TEXT("independent_readback"),
						Contract.GetReadbackId(),
						TEXT("native-state:") + Contract.GetCapabilityId()),
					{Entry}, Context.GetResourceTracker(), OutSnapshot, Error))
			{
				NativeContext(Error);
				OutError = FDiagnostic::WrapFailure(TEXT("rig_simulation_spatial_native_snapshot_failed"), Error, TEXT("native_execution"));
				return false;
			}
			return true;
		}

		FString SnapshotState(const FUeAgentCanonicalSnapshot& Snapshot)
		{
			if (Snapshot.GetEntries().IsEmpty()) return FString();
			const FUeAgentCanonicalValueRef& Root =
				Snapshot.GetEntries()[0].GetValue();
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Pair
				: Root->GetObjectItems())
			{
				if (Pair.Key == TEXT("observed_state"))
				{
					return Pair.Value->GetLexeme();
				}
			}
			return FString();
		}

		bool CheckRevision(
			IUeAgentRigSimulationSpatialNativeEnvironment& Environment,
			const FString& ExplicitRef,
			const FRigSimulationSpatialRevision& Expected,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			FRigSimulationSpatialRevision Actual;
			if (!Environment.TryReadRevision(ExplicitRef, Actual, OutError)) return false;
			if (Actual == Expected) return true;
			const bool bSource = Actual.SourceId != Expected.SourceId;
			const bool bGeneration = !bSource && Actual.Generation != Expected.Generation;
			NativeFailure(OutError, TEXT("rig_simulation_spatial_revision_mismatch"),
				bSource ? TEXT("The revision source differs from the expected source.")
					: bGeneration ? TEXT("The revision generation differs from the expected generation.")
					: TEXT("The revision sequence differs from the expected sequence."),
				bSource ? TEXT("/expected_revision/source_id") : bGeneration
					? TEXT("/expected_revision/generation") : TEXT("/expected_revision/sequence"),
				nullptr, nullptr, ExplicitRef);
			if (bSource || bGeneration)
			{
				OutError.ActualEvidence = MakeShared<FJsonValueString>(bSource ? Actual.SourceId : Actual.Generation);
				OutError.ExpectedEvidence = MakeShared<FJsonValueString>(bSource ? Expected.SourceId : Expected.Generation);
			}
			else
			{
				OutError.ActualValuePresent = true;
				OutError.ActualValue = MakeShared<FJsonValueString>(::LexToString(Actual.Sequence));
				OutError.ExpectedValuePresent = true;
				OutError.ExpectedValue = MakeShared<FJsonValueString>(::LexToString(Expected.Sequence));
			}
			return false;
		}

		FString ReadState(
			const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
			const FUeAgentRigSimulationSpatialRequest& Request,
			IUeAgentRigSimulationSpatialNativeEnvironment& Environment,
			FDiagnostic& OutDifference,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			if (Contract.GetCapabilityId()
				== TEXT("rig.skeleton.preview_mesh.bind"))
			{
				const auto& Input =
					Request.GetPayload().Get<FRigSkeletonPreviewMeshBindInput>();
				const auto* Binding = Environment.FindSkeletonBinding(Input.SkeletonRef);
				USkeleton* Skeleton = Binding ? Binding->Skeleton.Get() : nullptr;
				if (!Skeleton) return MissingNative(OutError,
					TEXT("rig_simulation_spatial_skeleton_binding_unavailable"),
					TEXT("The skeleton reference has no available native skeleton binding."),
					TEXT("/skeleton_binding"), Input.SkeletonRef), FString();
				USkeletalMesh* Preview = Skeleton->GetPreviewMesh(true);
				const bool bMatches = Preview
					&& Preview == Environment.ResolveExplicitObject(Input.PreviewMeshRef);
				FDiagnostic Difference;
				if (!MatchFact(bMatches, Difference,
					Preview ? TEXT("The skeleton preview mesh differs from the requested preview mesh.")
						: TEXT("The skeleton has no preview mesh."),
					TEXT("/preview_mesh_ref"), MakeShared<FJsonValueBoolean>(Preview != nullptr),
					MakeShared<FJsonValueString>(Input.PreviewMeshRef), Input.SkeletonRef))
				{
					if (Preview) Difference.ActualEvidence = MakeShared<FJsonValueString>(Preview->GetPathName());
					AppendDifference(OutDifference, MoveTemp(Difference));
				}
				return FString::Printf(TEXT("skeleton=%s|preview_ref=%s|preview=%s|matches=%d"),
					*Input.SkeletonRef, *Input.PreviewMeshRef,
					Preview ? *Preview->GetPathName() : TEXT("none"), bMatches ? 1 : 0);
			}
			if (Contract.GetCapabilityId() == TEXT("rig.skeleton.socket.update"))
			{
				const auto& Input =
					Request.GetPayload().Get<FRigSkeletonSocketUpdateInput>();
				const auto* SkeletonBinding =
					Environment.FindSkeletonBinding(Input.SkeletonRef);
				const auto* SocketBinding = Environment.FindSocketBinding(
					Input.SkeletonRef, Input.SocketRef);
				USkeleton* Skeleton = SkeletonBinding
					? SkeletonBinding->Skeleton.Get() : nullptr;
				if (!Skeleton || !SocketBinding) return MissingNative(OutError,
					TEXT("rig_simulation_spatial_socket_binding_unavailable"),
					!Skeleton ? TEXT("The socket's skeleton binding is unavailable.")
						: TEXT("The socket reference has no registered binding."),
					!Skeleton ? TEXT("/skeleton_binding") : TEXT("/socket_binding"), Input.SocketRef), FString();
				const USkeletalMeshSocket* Socket =
					Skeleton->FindSocket(SocketBinding->SocketName);
				const bool bParentMatches = Socket
					&& Socket->BoneName == SocketBinding->ParentBoneName;
				if (Input.Operation == TEXT("remove"))
					MatchFact(!Socket, OutDifference, TEXT("The removed socket is still present."),
						TEXT("/socket/present"), MakeShared<FJsonValueBoolean>(Socket != nullptr),
						MakeShared<FJsonValueBoolean>(false), Input.SocketRef);
				else if (MatchFact(Socket != nullptr, OutDifference, TEXT("The requested socket is absent."),
					TEXT("/socket/present"), MakeShared<FJsonValueBoolean>(Socket != nullptr),
					MakeShared<FJsonValueBoolean>(true), Input.SocketRef) && !bParentMatches)
				{
					FDiagnostic Difference;
					NativeFailure(Difference, TEXT("rig_simulation_spatial_native_state_mismatch"),
						TEXT("The socket's parent bone differs from its registered parent."),
						TEXT("/parent_bone_ref"), nullptr, MakeShared<FJsonValueString>(Input.ParentBoneRef), Input.SocketRef);
					Difference.ActualEvidence = MakeShared<FJsonValueString>(Socket->BoneName.ToString());
					Difference.ExpectedEvidence = MakeShared<FJsonValueString>(SocketBinding->ParentBoneName.ToString());
					AppendDifference(OutDifference, MoveTemp(Difference));
				}
				return FString::Printf(
					TEXT("skeleton=%s|socket_ref=%s|present=%d|parent_ref=%s|parent_matches=%d|%s"),
					*Input.SkeletonRef, *Input.SocketRef, Socket ? 1 : 0,
					*Input.ParentBoneRef, bParentMatches ? 1 : 0,
					Socket ? *TransformText(FTransform(Socket->RelativeRotation,
						Socket->RelativeLocation, Socket->RelativeScale)) : TEXT("none"));
			}
			if (Contract.GetCapabilityId() == TEXT("rig.control.bake.animation"))
			{
				const auto& Input =
					Request.GetPayload().Get<FRigControlBakeAnimationInput>();
				const UAnimSequence* Animation = Cast<UAnimSequence>(
					Environment.ResolveExplicitObject(Input.TargetAnimationRef));
				const auto* Binding = Environment.FindRigBakeBinding(Input.BindingRef);
				const IAnimationDataModel* Model = Animation
					? Animation->GetDataModel() : nullptr;
				TArray<FName> Names;
				if (Model) Model->GetBoneTrackNames(Names);
				const int32 KeyCount = Model ? Model->GetNumberOfKeys() : 0;
				if (MatchFact(Names.Num() != 0, OutDifference,
					TEXT("The baked animation has no bone tracks."), TEXT("/bone_tracks"),
					MakeShared<FJsonValueNumber>(Names.Num()), MakeShared<FJsonValueString>(TEXT("a nonzero count")), Input.TargetAnimationRef))
					MatchFact(KeyCount != 0, OutDifference, TEXT("The baked animation has no keys."),
						TEXT("/keys"), MakeShared<FJsonValueNumber>(KeyCount),
						MakeShared<FJsonValueString>(TEXT("a nonzero count")), Input.TargetAnimationRef);
				return FString::Printf(
					TEXT("animation=%s|binding=%s|clock=%s|bone_tracks=%d|keys=%d"),
					*Input.TargetAnimationRef, *Input.BindingRef,
					Binding ? *Binding->NativeAuthoredClockId : TEXT("none"), Names.Num(),
					KeyCount);
			}
			if (Contract.GetCapabilityId() == TEXT("rig.control.bake.controls"))
			{
				const auto& Input =
					Request.GetPayload().Get<FRigControlBakeControlsInput>();
				const auto* Binding = Environment.FindRigBakeBinding(Input.BindingRef);
				ULevelSequence* Sequence = Binding ? Binding->Sequence.Get() : nullptr;
				const int32 KeyCount = Sequence ? CountControlKeys(*Sequence) : 0;
				MatchFact(KeyCount != 0, OutDifference, TEXT("The sequence has no control keys."),
					TEXT("/control_keys"), MakeShared<FJsonValueNumber>(KeyCount),
					MakeShared<FJsonValueString>(TEXT("a nonzero count")), Input.TargetSequenceRef);
				return FString::Printf(
					TEXT("sequence=%s|binding=%s|clock=%s|control_frame=%s|control_keys=%d"),
					*Input.TargetSequenceRef, *Input.BindingRef,
					Binding ? *Binding->NativeAuthoredClockId : TEXT("none"),
					Binding ? *Binding->NativeControlFrameId : TEXT("none"),
					KeyCount);
			}
			UnsupportedCapability(OutError, Contract.GetCapabilityId());
			return FString();
		}

		bool ReadBakeContext(const FRigControlBakeContextInput& Input,
			IUeAgentRigSimulationSpatialNativeEnvironment& Environment,
			TSharedPtr<FJsonObject>& OutContext, FString& OutState,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			const auto* Binding = Environment.FindRigBakeBinding(Input.BindingName);
			ULevelSequence* Sequence = Cast<ULevelSequence>(Environment.ResolveExplicitObject(Input.SequenceRef));
			UClass* RigClass = Cast<UClass>(Environment.ResolveExplicitObject(Input.RigRef));
			UMovieScene* MovieScene = Sequence ? Sequence->GetMovieScene() : nullptr;
			if (!Binding || Binding->Sequence.Get() != Sequence || !MovieScene)
				return MissingNative(OutError, TEXT("rig_simulation_spatial_rig_bake_binding_unavailable"),
					TEXT("The requested sequence has no resolved skeletal binding."), TEXT("/binding_name"), Input.BindingName);
			if (!RigClass || !RigClass->IsChildOf(UControlRig::StaticClass()))
				return MissingNative(OutError, TEXT("rig_simulation_spatial_rig_bake_binding_unavailable"),
					TEXT("The rig reference must resolve to a Control Rig generated class."), TEXT("/rig_ref"), Input.RigRef);
			const TRange<FFrameNumber> Range = MovieScene->GetPlaybackRange();
			if (!Range.HasLowerBound() || !Range.HasUpperBound() || Range.IsEmpty())
				return MissingNative(OutError, TEXT("rig_simulation_spatial_bake_range_unavailable"),
					TEXT("The sequence requires a finite nonempty playback range for baking."), TEXT("/playback_range"), Input.SequenceRef);
			const auto RateJson = [](const FFrameRate& Rate)
			{
				auto Json = MakeShared<FJsonObject>();
				Json->SetStringField(TEXT("numerator"), ::LexToString(Rate.Numerator));
				Json->SetStringField(TEXT("denominator"), ::LexToString(Rate.Denominator));
				return Json;
			};
			const auto RevisionJson = [](const FRigSimulationSpatialRevision& Revision)
			{
				auto Json = MakeShared<FJsonObject>();
				Json->SetStringField(TEXT("source_id"), Revision.SourceId);
				Json->SetStringField(TEXT("generation"), Revision.Generation);
				Json->SetStringField(TEXT("sequence"), ::LexToString(Revision.Sequence));
				return Json;
			};
			auto AuthoredRange = MakeShared<FJsonObject>();
			AuthoredRange->SetStringField(TEXT("clock_id"), Binding->NativeAuthoredClockId);
			AuthoredRange->SetStringField(TEXT("lower_tick"), ::LexToString(UE::MovieScene::DiscreteInclusiveLower(Range).Value));
			AuthoredRange->SetStringField(TEXT("upper_tick"), ::LexToString(UE::MovieScene::DiscreteExclusiveUpper(Range).Value));
			AuthoredRange->SetObjectField(TEXT("rate"), RateJson(MovieScene->GetTickResolution()));
			AuthoredRange->SetStringField(TEXT("state"), TEXT("authored"));
			const auto CommonInput = [&]()
			{
				auto Json = MakeShared<FJsonObject>();
				Json->SetStringField(TEXT("binding_ref"), Binding->BindingGuid.ToString());
				Json->SetStringField(TEXT("rig_ref"), Input.RigRef);
				Json->SetObjectField(TEXT("authored_range"), AuthoredRange);
				Json->SetObjectField(TEXT("sample_rate"), RateJson(MovieScene->GetDisplayRate()));
				return Json;
			};
			FRigSimulationSpatialRevision SequenceRevision;
			if (!Environment.TryReadRevision(Input.SequenceRef, SequenceRevision, OutError)) return false;
			auto ControlsInput = CommonInput();
			ControlsInput->SetStringField(TEXT("target_sequence_ref"), Input.SequenceRef);
			ControlsInput->SetObjectField(TEXT("expected_revision"), RevisionJson(SequenceRevision));
			OutContext = MakeShared<FJsonObject>();
			OutContext->SetStringField(TEXT("sequence_ref"), Input.SequenceRef);
			OutContext->SetStringField(TEXT("binding_name"), Input.BindingName);
			OutContext->SetObjectField(TEXT("controls_input"), ControlsInput);
			OutState = FString::Printf(TEXT("sequence=%s|binding=%s|revision=%lld"),
				*Input.SequenceRef, *Input.BindingName, SequenceRevision.Sequence);
			if (!Input.TargetAnimationRef.IsEmpty())
			{
				if (!Cast<UAnimSequence>(Environment.ResolveExplicitObject(Input.TargetAnimationRef)))
					return MissingNative(OutError, TEXT("rig_simulation_spatial_rig_bake_binding_unavailable"),
						TEXT("The target animation must be an existing AnimSequence."), TEXT("/target_animation_ref"), Input.TargetAnimationRef);
				FRigSimulationSpatialRevision AnimationRevision;
				if (!Environment.TryReadRevision(Input.TargetAnimationRef, AnimationRevision, OutError)) return false;
				auto AnimationInput = CommonInput();
				AnimationInput->SetStringField(TEXT("source_sequence_ref"), Input.SequenceRef);
				AnimationInput->SetStringField(TEXT("target_animation_ref"), Input.TargetAnimationRef);
				AnimationInput->SetObjectField(TEXT("expected_revision"), RevisionJson(AnimationRevision));
				OutContext->SetObjectField(TEXT("animation_input"), AnimationInput);
				OutState += FString::Printf(TEXT("|animation=%s|revision=%lld"), *Input.TargetAnimationRef, AnimationRevision.Sequence);
			}
			return true;
		}

		class FReadback final : public IUeAgentRigSimulationSpatialReadbackPort
		{
		public:
			explicit FReadback(
				TSharedRef<IUeAgentRigSimulationSpatialNativeEnvironment> InEnvironment)
				: Environment(MoveTemp(InEnvironment)) {}

			bool TryRead(
				const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
				const FUeAgentRigSimulationSpatialTypedRequestRef& Request,
				const TOptional<FUeAgentCandidateBinding>& Target,
				EUeAgentRigSimulationSpatialReadPhase Phase,
				const FUeAgentOperationContext& Context,
				TOptional<FUeAgentRigSimulationSpatialReadbackResult>& OutReadback,
				FUeAgentRigSimulationSpatialExecutionError& OutError) override
			{
				OutReadback.Reset();
				TSharedPtr<const FUeAgentCanonicalSnapshot> OutSnapshot;
				OutError = {};
				Environment->CountFamilyDispatch();
				if (!IsImplementedCapability(Contract.GetCapabilityId()))
				{
					return UnsupportedCapability(OutError, Contract.GetCapabilityId());
				}
				const FUeAgentRigSimulationSpatialRequest* Typed =
					ProductionRequest(Request, OutError);
				if (!Typed) return false;
				const FString PublicRef = Typed->GetPrimaryTargetRef();
				if (PublicRef.IsEmpty()
					|| (Target.IsSet() && Target->GetPublicRef() != PublicRef))
				{
					return NativeFailure(OutError, TEXT("rig_simulation_spatial_explicit_target_mismatch"),
						PublicRef.IsEmpty() ? TEXT("The request has no primary target reference.")
							: TEXT("The readback target differs from the request's primary target."),
						TEXT("/primary_target_ref"),
						MakeShared<FJsonValueString>(PublicRef.IsEmpty() ? PublicRef : Target->GetPublicRef()),
						MakeShared<FJsonValueString>(PublicRef.IsEmpty() ? TEXT("a nonempty logical reference") : PublicRef), PublicRef);
				}
				FDiagnostic Difference;
				TSharedPtr<FJsonObject> BakeContext;
				FString State;
				if (Contract.GetCapabilityId() == TEXT("rig.control.bake.context"))
				{
					if (!ReadBakeContext(Typed->GetPayload().Get<FRigControlBakeContextInput>(),
						*Environment, BakeContext, State, OutError)) return false;
				}
				else State = ReadState(Contract, *Typed, *Environment, Difference, OutError);
				if (State.IsEmpty() || !TrySnapshot(Contract, Phase, PublicRef,
					State, Context, OutSnapshot, OutError, BakeContext)) return false;
				OutReadback.Emplace(OutSnapshot.ToSharedRef(), MoveTemp(Difference));
				return true;
			}

		private:
			TSharedRef<IUeAgentRigSimulationSpatialNativeEnvironment> Environment;
		};

		class FMutation final : public IUeAgentRigSimulationSpatialMutationPort
		{
		public:
			explicit FMutation(
				TSharedRef<IUeAgentRigSimulationSpatialNativeEnvironment> InEnvironment)
				: Environment(MoveTemp(InEnvironment)) {}

			bool TryMutate(
				const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
				const FUeAgentRigSimulationSpatialTypedRequestRef& Request,
				const TArray<FUeAgentOperationTargetBinding>& Targets,
				const FUeAgentOperationContext& Context,
				FUeAgentRigSimulationSpatialMutationResult& OutResult,
				FUeAgentRigSimulationSpatialExecutionError& OutError) override
			{
				OutResult = {};
				OutError = {};
				Environment->CountFamilyDispatch();
				if (!IsImplementedCapability(Contract.GetCapabilityId()))
				{
					return UnsupportedCapability(OutError, Contract.GetCapabilityId());
				}
				const FUeAgentRigSimulationSpatialRequest* Typed =
					ProductionRequest(Request, OutError);
				if (!Typed) return false;
				const TOptional<FUeAgentCandidateBinding> Target = FirstTarget(Targets);
				if (!Target.IsSet()
					|| Target->GetPublicRef() != Typed->GetPrimaryTargetRef())
				{
					return NativeFailure(OutError, TEXT("rig_simulation_spatial_explicit_target_mismatch"),
						!Target.IsSet() ? TEXT("The mutation has no resolved target binding.")
							: TEXT("The mutation target differs from the request's primary target."),
						TEXT("/primary_target_ref"), Target.IsSet() ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Target->GetPublicRef()))
							: TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()),
						MakeShared<FJsonValueString>(Typed->GetPrimaryTargetRef()), Typed->GetPrimaryTargetRef());
				}

				UObject* MutatedObject = nullptr;
				if (Contract.GetCapabilityId()
					== TEXT("rig.skeleton.preview_mesh.bind"))
				{
					const auto& Input = Typed->GetPayload()
						.Get<FRigSkeletonPreviewMeshBindInput>();
					const auto* Binding =
						Environment->FindSkeletonBinding(Input.SkeletonRef);
					USkeleton* Skeleton = Binding ? Binding->Skeleton.Get() : nullptr;
					USkeletalMesh* PreviewMesh = Cast<USkeletalMesh>(
						Environment->ResolveExplicitObject(Input.PreviewMeshRef));
					if (!Skeleton) return MissingNative(OutError, TEXT("rig_simulation_spatial_skeleton_preview_binding_unavailable"),
						TEXT("The skeleton reference has no available native skeleton binding."), TEXT("/skeleton_binding"), Input.SkeletonRef);
					if (!PreviewMesh) return MissingNative(OutError, TEXT("rig_simulation_spatial_skeleton_preview_binding_unavailable"),
						TEXT("The preview mesh reference does not resolve to a skeletal mesh."), TEXT("/preview_mesh_ref"), Input.PreviewMeshRef);
					UObject* ResolvedSkeleton = Environment->ResolveExplicitObject(Input.SkeletonRef);
					if (ResolvedSkeleton != Skeleton)
					{
						NativeFailure(OutError, TEXT("rig_simulation_spatial_skeleton_preview_binding_unavailable"),
							TEXT("The skeleton reference resolves to a different object than its native binding."), TEXT("/skeleton_ref"),
							MakeShared<FJsonValueBoolean>(ResolvedSkeleton != nullptr), MakeShared<FJsonValueString>(TEXT("the registered skeleton")), Input.SkeletonRef);
						if (ResolvedSkeleton) OutError.ActualEvidence = MakeShared<FJsonValueString>(ResolvedSkeleton->GetPathName());
						OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Skeleton->GetPathName());
						return false;
					}
					if (!CheckRevision(*Environment, Input.SkeletonRef,
							Input.ExpectedRevision, OutError)) return false;
					Skeleton->Modify();
					Skeleton->SetPreviewMesh(PreviewMesh, true);
					Skeleton->MarkPackageDirty();
					MutatedObject = Skeleton;
				}
				else if (Contract.GetCapabilityId()
					== TEXT("rig.skeleton.socket.update"))
				{
					const auto& Input = Typed->GetPayload()
						.Get<FRigSkeletonSocketUpdateInput>();
					const auto* SkeletonBinding =
						Environment->FindSkeletonBinding(Input.SkeletonRef);
					const auto* SocketBinding = Environment->FindSocketBinding(
						Input.SkeletonRef, Input.SocketRef);
					USkeleton* Skeleton = SkeletonBinding
						? SkeletonBinding->Skeleton.Get() : nullptr;
					if (!Skeleton || !SocketBinding) return MissingNative(OutError, TEXT("rig_simulation_spatial_socket_binding_unavailable"),
						!Skeleton ? TEXT("The socket's skeleton binding is unavailable.") : TEXT("The socket has no registered binding."),
						!Skeleton ? TEXT("/skeleton_binding") : TEXT("/socket_binding"), Input.SocketRef);
					if (Input.ParentBoneRef != SocketBinding->ParentBoneIdentity)
						return InputFailure(OutError, TEXT("rig_simulation_spatial_socket_binding_unavailable"),
							TEXT("The requested parent bone differs from the socket binding."), TEXT("/parent_bone_ref"),
							MakeShared<FJsonValueString>(Input.ParentBoneRef), MakeShared<FJsonValueString>(SocketBinding->ParentBoneIdentity), Input.SocketRef);
					if (Skeleton->GetReferenceSkeleton().FindBoneIndex(SocketBinding->ParentBoneName) == INDEX_NONE)
					{
						MissingNative(OutError, TEXT("rig_simulation_spatial_socket_binding_unavailable"),
							TEXT("The registered parent bone is absent from the skeleton."), TEXT("/parent_bone"), Input.ParentBoneRef);
						OutError.ActualEvidence = MakeShared<FJsonValueString>(SocketBinding->ParentBoneName.ToString());
						return false;
					}
					if (!CheckRevision(*Environment, Input.SkeletonRef,
							Input.ExpectedRevision, OutError)) return false;
					USkeletalMeshSocket* Socket =
						Skeleton->FindSocket(SocketBinding->SocketName);
					Skeleton->Modify();
					if (Input.Operation == TEXT("remove"))
					{
						if (!Socket) return MissingNative(OutError, TEXT("rig_simulation_spatial_socket_not_found"),
							TEXT("The socket requested for removal is absent."), TEXT("/socket/present"), Input.SocketRef);
						Skeleton->Sockets.Remove(Socket);
					}
					else
					{
						if ((Input.Operation == TEXT("create") && Socket)
							|| (Input.Operation == TEXT("update") && !Socket))
						{
							return NativeFailure(OutError,
								Input.Operation == TEXT("create") ? TEXT("rig_simulation_spatial_socket_already_exists")
									: TEXT("rig_simulation_spatial_socket_not_found"),
								Input.Operation == TEXT("create") ? TEXT("A socket requested for creation already exists.")
									: TEXT("The socket requested for update is absent."), TEXT("/socket/present"),
								MakeShared<FJsonValueBoolean>(Socket != nullptr),
								MakeShared<FJsonValueBoolean>(Input.Operation != TEXT("create")), Input.SocketRef);
						}
						if (!Socket)
						{
							Socket = NewObject<USkeletalMeshSocket>(
								Skeleton, NAME_None, RF_Transactional);
							if (!Socket) return MissingNative(OutError, TEXT("rig_simulation_spatial_socket_create_failed"),
								TEXT("Creating the native skeleton socket returned no object."), TEXT("/socket"), Input.SocketRef);
							Skeleton->Sockets.Add(Socket);
						}
						const TArray<FUeAgentSpatialValuePtr>& Values =
							Typed->GetFormalPortRequest().GetSpatialValues();
						FUeAgentSpatialValuePtr Converted;
						FTransform NativeTransform;
						if (Values.Num() != 1)
							return NativeFailure(OutError, TEXT("rig_simulation_spatial_socket_transform_invalid"),
								TEXT("The formal socket request requires exactly one spatial value."), TEXT("/formal_request/spatial_values"),
								MakeShared<FJsonValueNumber>(Values.Num()), MakeShared<FJsonValueNumber>(1), Input.SocketRef);
						if (!Values[0].IsValid())
							return MissingNative(OutError, TEXT("rig_simulation_spatial_socket_transform_invalid"),
								TEXT("The formal socket spatial value is absent."), TEXT("/formal_request/spatial_values/0"), Input.SocketRef);
						if (Values[0]->GetSubject() != Input.SocketRef)
						{
							NativeFailure(OutError, TEXT("rig_simulation_spatial_socket_transform_invalid"),
								TEXT("The formal transform belongs to a different subject."), TEXT("/formal_request/spatial_values/0/subject"),
								nullptr, MakeShared<FJsonValueString>(Input.SocketRef), Input.SocketRef);
							OutError.ActualEvidence = MakeShared<FJsonValueString>(Values[0]->GetSubject());
							return false;
						}
						if (!Environment->TryConvertSpatialValueToFrame(*Values[0], SocketBinding->NativeParentFrameId,
							Context.GetResourceTracker(), Converted, OutError)) return false;
						if (!Converted.IsValid())
							return MissingNative(OutError, TEXT("rig_simulation_spatial_socket_transform_invalid"),
								TEXT("Spatial conversion returned no socket transform."), TEXT("/converted_transform"), Input.SocketRef);
						if (!TryNativeTransform(*Converted, NativeTransform, OutError))
						{
							if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Input.SocketRef;
							return false;
						}
						Socket->SocketName = SocketBinding->SocketName;
						Socket->BoneName = SocketBinding->ParentBoneName;
						Socket->RelativeLocation = NativeTransform.GetTranslation();
						Socket->RelativeRotation = NativeTransform.Rotator();
						Socket->RelativeScale = NativeTransform.GetScale3D();
					}
					Skeleton->MarkPackageDirty();
					MutatedObject = Skeleton;
				}
				else if (Contract.GetCapabilityId()
					== TEXT("rig.control.bake.animation"))
				{
					const auto& Input = Typed->GetPayload()
						.Get<FRigControlBakeAnimationInput>();
					const auto* Binding =
						Environment->FindRigBakeBinding(Input.BindingRef);
					ULevelSequence* Sequence = Cast<ULevelSequence>(
						Environment->ResolveExplicitObject(Input.SourceSequenceRef));
					UAnimSequence* Animation = Cast<UAnimSequence>(
						Environment->ResolveExplicitObject(Input.TargetAnimationRef));
					UClass* RigClass = Cast<UClass>(
						Environment->ResolveExplicitObject(Input.RigRef));
					if (!Binding) return MissingNative(OutError, TEXT("rig_simulation_spatial_rig_bake_binding_unavailable"),
						TEXT("The bake binding is not registered."), TEXT("/binding"), Input.BindingRef);
					if (!Sequence) return MissingNative(OutError, TEXT("rig_simulation_spatial_rig_bake_binding_unavailable"),
						TEXT("The source sequence reference does not resolve to a level sequence."), TEXT("/source_sequence_ref"), Input.SourceSequenceRef);
					if (Binding->Sequence.Get() != Sequence)
						return NativeFailure(OutError, TEXT("rig_simulation_spatial_rig_bake_binding_unavailable"),
							TEXT("The source sequence differs from the bake binding's sequence."), TEXT("/binding/sequence"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Input.SourceSequenceRef);
					if (!Binding->World.IsValid()) return MissingNative(OutError, TEXT("rig_simulation_spatial_rig_bake_binding_unavailable"),
						TEXT("The bake binding has no available world."), TEXT("/binding/world"), Input.BindingRef);
					if (!Animation) return MissingNative(OutError, TEXT("rig_simulation_spatial_rig_bake_binding_unavailable"),
						TEXT("The target animation reference does not resolve to an animation sequence."), TEXT("/target_animation_ref"), Input.TargetAnimationRef);
					if (!RigClass) return MissingNative(OutError, TEXT("rig_simulation_spatial_rig_bake_binding_unavailable"),
						TEXT("The rig reference does not resolve to a class."), TEXT("/rig_ref"), Input.RigRef);
					if (!RigClass->IsChildOf(UControlRig::StaticClass()))
					{
						NativeFailure(OutError, TEXT("rig_simulation_spatial_rig_bake_binding_unavailable"),
							TEXT("The resolved rig class does not derive from ControlRig."), TEXT("/rig_class"),
							nullptr, MakeShared<FJsonValueString>(TEXT("a ControlRig subclass")), Input.RigRef);
						OutError.ActualEvidence = MakeShared<FJsonValueString>(RigClass->GetPathName());
						return false;
					}
					if (!CheckRevision(*Environment, Input.TargetAnimationRef,
							Input.ExpectedRevision, OutError)) return false;
					TOptional<FUeAgentTemporalRange> NativeRange;
					FFrameRate SampleRate;
					int32 Lower = 0;
					int32 Upper = 0;
					FFrameRate DisplayRate;
					const auto& Ranges = Typed->GetFormalPortRequest().GetTemporalRanges();
					if (Ranges.Num() != 1)
						return NativeFailure(OutError, TEXT("rig_simulation_spatial_native_authored_range_invalid"),
							TEXT("The formal bake request requires exactly one temporal range."), TEXT("/formal_request/temporal_ranges"),
							MakeShared<FJsonValueNumber>(Ranges.Num()), MakeShared<FJsonValueNumber>(1), Input.BindingRef);
					if (!Environment->TryConvertTemporalRangeToClock(Ranges[0], Binding->NativeAuthoredClockId,
						Context.GetResourceTracker(), NativeRange, OutError)) return false;
					if (!NativeRange.IsSet())
						return MissingNative(OutError, TEXT("rig_simulation_spatial_native_authored_range_invalid"),
							TEXT("Temporal conversion returned no native range."), TEXT("/converted_range"), Input.BindingRef);
					if (!TryRangeFrames(*NativeRange, Lower, Upper, DisplayRate, OutError)
						|| !TryRate(Input.SampleRate, SampleRate, OutError))
					{
						if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Input.BindingRef;
						return false;
					}
					UAnimSeqExportOption* Options = NewObject<UAnimSeqExportOption>(
						GetTransientPackage(), NAME_None, RF_Transient);
					Options->bUseCustomTimeRange = true;
					Options->CustomStartFrame = Lower;
					Options->CustomEndFrame = Upper;
					Options->CustomDisplayRate = DisplayRate;
					Options->bUseCustomFrameRate = true;
					Options->CustomFrameRate = SampleRate;
					Animation->Modify();
					if (!USequencerToolsFunctionLibrary::ExportAnimSequence(
							Binding->World.Get(), Sequence, Animation, Options,
							FMovieSceneBindingProxy(Binding->BindingGuid, Sequence), false))
					{
						return NativeFailure(OutError, TEXT("rig_simulation_spatial_bake_animation_failed"),
							TEXT("Sequencer failed to export the source sequence into the target animation."),
							TEXT("/animation_export"), MakeShared<FJsonValueBoolean>(false),
							MakeShared<FJsonValueBoolean>(true), Input.TargetAnimationRef);
					}
					const IAnimationDataModel* Model = Animation->GetDataModel();
					if (!Model) return MissingNative(OutError, TEXT("rig_simulation_spatial_bake_animation_empty"),
						TEXT("The exported animation has no data model."), TEXT("/animation/data_model"), Input.TargetAnimationRef);
					const int32 BoneTrackCount = Model->GetNumBoneTracks();
					if (BoneTrackCount <= 0)
						return NativeFailure(OutError, TEXT("rig_simulation_spatial_bake_animation_empty"),
							TEXT("The exported animation has no bone tracks."), TEXT("/animation/bone_tracks"),
							MakeShared<FJsonValueNumber>(BoneTrackCount), MakeShared<FJsonValueString>(TEXT("> 0")), Input.TargetAnimationRef);
					const int32 KeyCount = Model->GetNumberOfKeys();
					if (KeyCount <= 0)
						return NativeFailure(OutError, TEXT("rig_simulation_spatial_bake_animation_empty"),
							TEXT("The exported animation has no keys."), TEXT("/animation/keys"),
							MakeShared<FJsonValueNumber>(KeyCount), MakeShared<FJsonValueString>(TEXT("> 0")), Input.TargetAnimationRef);
					Animation->MarkPackageDirty();
					MutatedObject = Animation;
				}
				else if (Contract.GetCapabilityId()
					== TEXT("rig.control.bake.controls"))
				{
					const auto& Input = Typed->GetPayload()
						.Get<FRigControlBakeControlsInput>();
					const auto* Binding =
						Environment->FindRigBakeBinding(Input.BindingRef);
					ULevelSequence* Sequence = Cast<ULevelSequence>(
						Environment->ResolveExplicitObject(Input.TargetSequenceRef));
					UClass* RigClass = Cast<UClass>(
						Environment->ResolveExplicitObject(Input.RigRef));
					const UControlRig* DefaultRig = RigClass
						? RigClass->GetDefaultObject<UControlRig>() : nullptr;
					const bool bSupported = RigClass
						&& (RigClass->IsChildOf(UFKControlRig::StaticClass())
							|| (DefaultRig && (DefaultRig->SupportsEvent(
								FRigUnit_InverseExecution::EventName)
								|| DefaultRig->SupportsEvent(TEXT("Inverse")))));
					USkeletalMeshComponent* SkeletalMeshComponent = Binding
						? Binding->SkeletalMeshComponent.Get() : nullptr;
					if (!Binding) return MissingNative(OutError, TEXT("rig_simulation_spatial_rig_bake_binding_unavailable"),
						TEXT("The bake binding is not registered."), TEXT("/binding"), Input.BindingRef);
					if (!Sequence) return MissingNative(OutError, TEXT("rig_simulation_spatial_rig_bake_binding_unavailable"),
						TEXT("The target sequence reference does not resolve to a level sequence."), TEXT("/target_sequence_ref"), Input.TargetSequenceRef);
					if (Binding->Sequence.Get() != Sequence)
						return NativeFailure(OutError, TEXT("rig_simulation_spatial_rig_bake_binding_unavailable"),
							TEXT("The target sequence differs from the bake binding's sequence."), TEXT("/binding/sequence"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Input.TargetSequenceRef);
					if (!Binding->World.IsValid()) return MissingNative(OutError, TEXT("rig_simulation_spatial_rig_bake_binding_unavailable"),
						TEXT("The bake binding has no available world."), TEXT("/binding/world"), Input.BindingRef);
					if (!SkeletalMeshComponent) return MissingNative(OutError, TEXT("rig_simulation_spatial_rig_bake_binding_unavailable"),
						TEXT("The bake binding has no available skeletal mesh component."), TEXT("/binding/skeletal_mesh_component"), Input.BindingRef);
					if (!SkeletalMeshComponent->GetSkeletalMeshAsset()) return MissingNative(OutError, TEXT("rig_simulation_spatial_rig_bake_binding_unavailable"),
						TEXT("The bound component has no skeletal mesh asset."), TEXT("/binding/skeletal_mesh"), Input.BindingRef);
					if (!bSupported)
						return NativeFailure(OutError, TEXT("rig_simulation_spatial_rig_bake_binding_unavailable"),
							TEXT("The rig does not support inverse evaluation required for baking controls."), TEXT("/rig/inverse_evaluation"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueString>(TEXT("FKControlRig or a rig supporting inverse execution")), Input.RigRef);
					if (!CheckRevision(*Environment, Input.TargetSequenceRef,
							Input.ExpectedRevision, OutError)) return false;
					TOptional<FUeAgentTemporalRange> NativeRange;
					FFrameRate SampleRate;
					int32 Lower = 0;
					int32 Upper = 0;
					FFrameRate DisplayRate;
					const auto& Ranges = Typed->GetFormalPortRequest().GetTemporalRanges();
					if (Ranges.Num() != 1)
						return NativeFailure(OutError, TEXT("rig_simulation_spatial_native_authored_range_invalid"),
							TEXT("The formal bake request requires exactly one temporal range."), TEXT("/formal_request/temporal_ranges"),
							MakeShared<FJsonValueNumber>(Ranges.Num()), MakeShared<FJsonValueNumber>(1), Input.BindingRef);
					if (!Environment->TryConvertTemporalRangeToClock(Ranges[0], Binding->NativeAuthoredClockId,
						Context.GetResourceTracker(), NativeRange, OutError)) return false;
					if (!NativeRange.IsSet())
						return MissingNative(OutError, TEXT("rig_simulation_spatial_native_authored_range_invalid"),
							TEXT("Temporal conversion returned no native range."), TEXT("/converted_range"), Input.BindingRef);
					if (!TryRangeFrames(*NativeRange, Lower, Upper, DisplayRate, OutError)
						|| !TryRate(Input.SampleRate, SampleRate, OutError))
					{
						if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Input.BindingRef;
						return false;
					}
					UAnimSeqExportOption* Options = NewObject<UAnimSeqExportOption>(
						GetTransientPackage(), NAME_None, RF_Transient);
					Options->bUseCustomTimeRange = true;
					Options->CustomStartFrame = Lower;
					Options->CustomEndFrame = Upper;
					Options->CustomDisplayRate = DisplayRate;
					Options->bUseCustomFrameRate = true;
					Options->CustomFrameRate = SampleRate;
					UAnimSequence* TemporaryAnimation = NewObject<UAnimSequence>(
						GetTransientPackage(), NAME_None, RF_Transient);
					TemporaryAnimation->SetSkeleton(
						SkeletalMeshComponent->GetSkeletalMeshAsset()->GetSkeleton());
					if (!USequencerToolsFunctionLibrary::ExportAnimSequence(
							Binding->World.Get(), Sequence, TemporaryAnimation, Options,
							FMovieSceneBindingProxy(Binding->BindingGuid, Sequence), false))
					{
						return NativeFailure(OutError, TEXT("rig_simulation_spatial_bake_controls_export_failed"),
							TEXT("Sequencer failed to export the temporary animation used for control baking."),
							TEXT("/temporary_animation_export"), MakeShared<FJsonValueBoolean>(false),
							MakeShared<FJsonValueBoolean>(true), Input.TargetSequenceRef);
					}
					UMovieScene* MovieScene = Sequence->GetMovieScene();
					UMovieSceneControlRigParameterTrack* Track = MovieScene
						? MovieScene->FindTrack<UMovieSceneControlRigParameterTrack>(
							Binding->BindingGuid) : nullptr;
					if (!Track && MovieScene)
					{
						Track = MovieScene->AddTrack<UMovieSceneControlRigParameterTrack>(
							Binding->BindingGuid);
					}
					if (!MovieScene || !Track) return MissingNative(OutError, TEXT("rig_simulation_spatial_control_track_create_failed"),
						!MovieScene ? TEXT("The sequence has no movie scene.") : TEXT("Creating the Control Rig parameter track failed."),
						!MovieScene ? TEXT("/movie_scene") : TEXT("/control_rig_track"), Input.TargetSequenceRef);
					Sequence->Modify();
					MovieScene->Modify();
					Track->Modify();
					Track->RemoveAllAnimationData();
					FString RigName = RigClass->GetName();
					RigName.RemoveFromEnd(TEXT("_C"));
					UControlRig* ControlRig = NewObject<UControlRig>(
						Track, RigClass, FName(*RigName), RF_Transactional);
					if (!ControlRig) return MissingNative(OutError, TEXT("rig_simulation_spatial_control_rig_create_failed"),
						TEXT("Creating the native Control Rig instance returned no object."), TEXT("/control_rig"), Input.RigRef);
					ControlRig->SetObjectBinding(MakeShared<FControlRigObjectBinding>());
					ControlRig->GetObjectBinding()->BindToObject(SkeletalMeshComponent);
					ControlRig->GetDataSourceRegistry()->RegisterDataSource(
						UControlRig::OwnerComponent,
						ControlRig->GetObjectBinding()->GetBoundObject());
					ControlRig->Initialize();
					ControlRig->RequestInit();
					ControlRig->SetBoneInitialTransformsFromSkeletalMeshComponent(
						SkeletalMeshComponent, true);
					ControlRig->Evaluate_AnyThread();
					UMovieSceneSection* RawSection = Track->CreateControlRigSection(
						0, ControlRig, true);
					if (!RawSection
						|| !UControlRigSequencerEditorLibrary::
							LoadAnimSequenceIntoControlRigSectionWithRange(
								RawSection, TemporaryAnimation, SkeletalMeshComponent,
								FFrameNumber(Lower), false, FFrameNumber(0),
								FFrameNumber(0), EMovieSceneTimeUnit::DisplayRate, false,
								0.001f, EMovieSceneKeyInterpolation::SmartAuto, true, false))
					{
						return NativeFailure(OutError, TEXT("rig_simulation_spatial_bake_controls_load_failed"),
							!RawSection ? TEXT("Creating the Control Rig section returned no section.")
								: TEXT("Loading the exported animation into the Control Rig section failed."),
							!RawSection ? TEXT("/control_rig_section") : TEXT("/control_rig_section/animation_load"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Input.TargetSequenceRef);
					}
					if (const FMovieSceneBinding* MovieBinding =
						MovieScene->FindBinding(Binding->BindingGuid))
					{
						for (UMovieSceneTrack* ExistingTrack : MovieBinding->GetTracks())
						{
							if (UMovieSceneSkeletalAnimationTrack* SkeletalTrack =
								Cast<UMovieSceneSkeletalAnimationTrack>(ExistingTrack))
							{
								for (UMovieSceneSection* Section : SkeletalTrack->GetAllSections())
								{
									if (Section) Section->SetIsActive(false);
								}
							}
						}
					}
					const int32 KeyCount = CountControlKeys(*Sequence);
					if (KeyCount <= 0)
						return NativeFailure(OutError, TEXT("rig_simulation_spatial_bake_controls_empty"),
							TEXT("The baked sequence has no control keys."), TEXT("/control_keys"),
							MakeShared<FJsonValueNumber>(KeyCount), MakeShared<FJsonValueString>(TEXT("> 0")), Input.TargetSequenceRef);
					Sequence->MarkPackageDirty();
					MutatedObject = Sequence;
				}

				if (!IsValid(MutatedObject)) return MissingNative(OutError, TEXT("rig_simulation_spatial_native_mutation_target_invalid"),
					TEXT("The native mutation produced no valid target object."), TEXT("/mutation/target"), Typed->GetPrimaryTargetRef());
				const FString OperationId = Context.GetRequestId() + TEXT(":")
					+ Contract.GetCapabilityId();
				Environment->RecordMutation(OperationId, MutatedObject);
				OutResult = FUeAgentRigSimulationSpatialMutationResult(OperationId,
					TEXT("native-mutation:") + MutatedObject->GetPathName());
				// Both constructor inputs contain nonempty fixed prefixes.
				return true;
			}

		private:
			TSharedRef<IUeAgentRigSimulationSpatialNativeEnvironment> Environment;
		};

		class FSettle final : public IUeAgentRigSimulationSpatialSettlePort
		{
		public:
			explicit FSettle(
				TSharedRef<IUeAgentRigSimulationSpatialNativeEnvironment> InEnvironment)
				: Environment(MoveTemp(InEnvironment)) {}

			bool TrySettle(
				const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
				const FUeAgentRigSimulationSpatialMutationResult& Mutation,
				const FUeAgentOperationContext& Context,
				FUeAgentRigSimulationSpatialExecutionError& OutError) override
			{
				OutError = {};
				if (!IsImplementedCapability(Contract.GetCapabilityId()))
				{
					return UnsupportedCapability(OutError, Contract.GetCapabilityId());
				}
				UObject* Object = Environment->FindMutation(Mutation.GetOperationId());
				if (!IsValid(Object))
				{
					MissingNative(OutError, TEXT("rig_simulation_spatial_settle_target_unavailable"),
						TEXT("The recorded mutation has no available native object to settle."), TEXT("/mutation/target"), FString());
					OutError.SubjectId = Mutation.GetOperationId();
					return false;
				}
				if (UAnimSequence* Animation = Cast<UAnimSequence>(Object))
				{
					Animation->RefreshCacheData();
				}
				else if (ULevelSequence* Sequence = Cast<ULevelSequence>(Object))
				{
					UMovieSceneCompiledDataManager::GetPrecompiledData()->Compile(Sequence);
				}
				Object->PostEditChange();
				return TrySaveProjectAsset(*Object, OutError);
			}

		private:
			TSharedRef<IUeAgentRigSimulationSpatialNativeEnvironment> Environment;
		};

		class FComparator final
			: public IUeAgentRigSimulationSpatialResultComparator
		{
		public:
			bool TryCompare(
				const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
				const FUeAgentRigSimulationSpatialTypedRequestRef& Request,
				const FUeAgentCanonicalSnapshot& Before,
				const FUeAgentRigSimulationSpatialReadbackResult& AfterReadback,
				const FUeAgentOperationContext& Context,
				FVerificationOutcome& OutOutcome,
				FUeAgentRigSimulationSpatialExecutionError& OutError) override
			{
				const auto& After = AfterReadback.Snapshot.Get();
				OutError = {};
				if (!IsImplementedCapability(Contract.GetCapabilityId()))
				{
					return UnsupportedCapability(OutError, Contract.GetCapabilityId());
				}
				const FUeAgentRigSimulationSpatialRequest* Typed =
					ProductionRequest(Request, OutError);
				if (!Typed) return false;
				const FString BeforeState = SnapshotState(Before);
				const FString AfterState = SnapshotState(After);
				FDiagnostic Difference;
				if (MatchFact(!BeforeState.IsEmpty(), Difference, TEXT("The before snapshot has no observed state."),
					TEXT("/before/observed_state"), MakeShared<FJsonValueBoolean>(!BeforeState.IsEmpty()),
					MakeShared<FJsonValueBoolean>(true), Typed->GetPrimaryTargetRef()))
					MatchFact(!AfterState.IsEmpty(), Difference, TEXT("The after snapshot has no observed state."),
						TEXT("/after/observed_state"), MakeShared<FJsonValueBoolean>(!AfterState.IsEmpty()),
						MakeShared<FJsonValueBoolean>(true), Typed->GetPrimaryTargetRef());
				if (!AfterReadback.Difference.IsEmpty()) AppendDifference(Difference, AfterReadback.Difference);
				const bool bMatches = Difference.IsEmpty();
				OutOutcome = FVerificationOutcome(TEXT("rig-simulation-spatial-verify:")
						+ Contract.GetCapabilityId(),
				bMatches ? EVerificationStatus::Verified
						: EVerificationStatus::Mismatch,
				bMatches ? EEffectOutcome::Complete : EEffectOutcome::Partial,
				MoveTemp(Difference),
				false,
				{},
				{TEXT("independent-state:") + After.GetSnapshotId()},
				{});
				return true;
			}
		};
	}

	namespace
	{
		bool NativeBindingFailure(Contract::FDiagnostic& OutError, const TCHAR* Code, const TCHAR* Reason,
			const TCHAR* Path, const FString& Identity, TSharedPtr<FJsonValue> Actual, const TCHAR* Expected)
		{
			OutError = Contract::FDiagnostic::Failure(Code, Reason, TEXT("native_binding"));
			OutError.PropertyPath = Path;
			OutError.TargetPath = Identity;
			OutError.ActualValuePresent = Actual.IsValid();
			OutError.ActualValue = MoveTemp(Actual);
			OutError.ExpectedValue = MakeShared<FJsonValueString>(Expected);
			return false;
		}
	}

	bool FUeAgentRigSimulationSpatialSkeletonBinding::TryValidate(Contract::FDiagnostic& OutError) const
	{
		OutError.Reset();
		if (Identity.IsEmpty()) return NativeBindingFailure(OutError, TEXT("rig_skeleton_binding_invalid"),
			TEXT("The skeleton binding requires a non-empty object reference."), TEXT("identity"), Identity,
			MakeShared<FJsonValueString>(Identity), TEXT("non-empty object reference"));
		if (!Skeleton.IsValid()) return NativeBindingFailure(OutError, TEXT("rig_skeleton_binding_invalid"),
			TEXT("The bound skeleton object is unavailable."), TEXT("skeleton"), Identity, nullptr, TEXT("valid skeleton object"));
		return true;
	}

	bool FUeAgentRigSimulationSpatialSkeletonBinding::IsValid() const
	{
		Contract::FDiagnostic Error;
		return TryValidate(Error);
	}

	bool FUeAgentRigSimulationSpatialSocketBinding::TryValidate(Contract::FDiagnostic& OutError) const
	{
		OutError.Reset();
		const auto Missing = [&](const TCHAR* Reason, const TCHAR* Path, const FString& Value, const TCHAR* Expected)
		{
			return NativeBindingFailure(OutError, TEXT("rig_socket_binding_invalid"), Reason, Path, SkeletonIdentity,
				MakeShared<FJsonValueString>(Value), Expected);
		};
		if (SkeletonIdentity.IsEmpty()) return Missing(TEXT("The socket binding requires a skeleton reference."),
			TEXT("skeleton_identity"), SkeletonIdentity, TEXT("non-empty skeleton reference"));
		if (SocketIdentity.IsEmpty()) return Missing(TEXT("The socket binding requires a socket reference."),
			TEXT("socket_identity"), SocketIdentity, TEXT("non-empty socket reference"));
		if (SocketName.IsNone()) return Missing(TEXT("The socket name must not be None."),
			TEXT("socket_name"), SocketName.ToString(), TEXT("socket name other than None"));
		if (ParentBoneIdentity.IsEmpty()) return Missing(TEXT("The socket binding requires a parent bone reference."),
			TEXT("parent_bone_identity"), ParentBoneIdentity, TEXT("non-empty bone reference"));
		if (ParentBoneName.IsNone()) return Missing(TEXT("The parent bone name must not be None."),
			TEXT("parent_bone_name"), ParentBoneName.ToString(), TEXT("bone name other than None"));
		if (NativeParentFrameId.IsEmpty()) return Missing(TEXT("The socket binding requires a parent frame identifier."),
			TEXT("native_parent_frame_id"), NativeParentFrameId, TEXT("non-empty frame identifier"));
		return true;
	}

	bool FUeAgentRigSimulationSpatialSocketBinding::IsValid() const
	{
		Contract::FDiagnostic Error;
		return TryValidate(Error);
	}

	bool FUeAgentRigSimulationSpatialRigBakeBinding::TryValidate(Contract::FDiagnostic& OutError) const
	{
		OutError.Reset();
		const auto Reject = [&](const TCHAR* Reason, const TCHAR* Path, TSharedPtr<FJsonValue> Actual, const TCHAR* Expected)
		{
			return NativeBindingFailure(OutError, TEXT("rig_bake_binding_invalid"), Reason, Path, Identity, MoveTemp(Actual), Expected);
		};
		if (Identity.IsEmpty()) return Reject(TEXT("The rig bake binding requires a binding reference."),
			TEXT("identity"), MakeShared<FJsonValueString>(Identity), TEXT("non-empty binding reference"));
		if (!World.IsValid()) return Reject(TEXT("The rig bake world is unavailable."), TEXT("world"), nullptr, TEXT("valid world"));
		if (!Sequence.IsValid()) return Reject(TEXT("The rig bake sequence is unavailable."), TEXT("sequence"), nullptr, TEXT("valid level sequence"));
		if (!SkeletalMeshComponent.IsValid()) return Reject(TEXT("The rig bake skeletal mesh component is unavailable."),
			TEXT("skeletal_mesh_component"), nullptr, TEXT("valid skeletal mesh component"));
		if (!BindingGuid.IsValid()) return Reject(TEXT("The rig bake sequence binding GUID is invalid."),
			TEXT("binding_guid"), MakeShared<FJsonValueString>(BindingGuid.ToString()), TEXT("non-zero binding GUID"));
		if (NativeAuthoredClockId.IsEmpty()) return Reject(TEXT("The rig bake binding requires an authored clock identifier."),
			TEXT("native_authored_clock_id"), MakeShared<FJsonValueString>(NativeAuthoredClockId), TEXT("non-empty clock identifier"));
		if (NativeControlFrameId.IsEmpty()) return Reject(TEXT("The rig bake binding requires a control frame identifier."),
			TEXT("native_control_frame_id"), MakeShared<FJsonValueString>(NativeControlFrameId), TEXT("non-empty frame identifier"));
		return true;
	}

	bool FUeAgentRigSimulationSpatialRigBakeBinding::IsValid() const
	{
		Contract::FDiagnostic Error;
		return TryValidate(Error);
	}

	bool FUeAgentRigSimulationSpatialNativePorts::TryValidate(Contract::FDiagnostic& OutError) const
	{
		OutError.Reset();
		const auto Missing = [&](const TCHAR* Reason, const TCHAR* Path, const TCHAR* Expected)
		{
			return NativeBindingFailure(OutError, TEXT("rig_simulation_spatial_native_ports_invalid"),
				Reason, Path, FString(), nullptr, Expected);
		};
		if (SupportedCapabilities.IsEmpty()) return NativeBindingFailure(OutError,
			TEXT("rig_simulation_spatial_native_ports_invalid"), TEXT("The native ports provide no capabilities."),
			TEXT("supported_capabilities"), FString(), MakeShared<FJsonValueNumber>(0), TEXT("at least one capability"));
		if (!Readback.IsValid()) return Missing(TEXT("The native readback port is unavailable."), TEXT("readback"), TEXT("valid readback port"));
		if (!Mutation.IsValid()) return Missing(TEXT("The native mutation port is unavailable."), TEXT("mutation"), TEXT("valid mutation port"));
		if (!Settle.IsValid()) return Missing(TEXT("The native settle port is unavailable."), TEXT("settle"), TEXT("valid settle port"));
		if (!Comparator.IsValid()) return Missing(TEXT("The native result comparator is unavailable."), TEXT("comparator"), TEXT("valid result comparator"));
		return true;
	}

	bool FUeAgentRigSimulationSpatialNativePorts::IsValid() const
	{
		Contract::FDiagnostic Error;
		return TryValidate(Error);
	}

	bool FUeAgentRigSimulationSpatialNativePorts::Supports(
		const FString& CapabilityId) const
	{
		return SupportedCapabilities.Contains(CapabilityId);
	}

	bool TryCreateRigSimulationSpatialSkeletalRigBakeNativePorts(
		TSharedRef<IUeAgentRigSimulationSpatialNativeEnvironment> Environment,
		FUeAgentRigSimulationSpatialNativePorts& OutPorts,
		FUeAgentRigSimulationSpatialExecutionError& OutError)
	{
		OutPorts = {};
		OutError = {};
		OutPorts.SupportedCapabilities = {
			TEXT("rig.skeleton.preview_mesh.bind"),
			TEXT("rig.skeleton.socket.update"),
			TEXT("rig.control.bake.context"),
			TEXT("rig.control.bake.animation"),
			TEXT("rig.control.bake.controls"),
		};
		OutPorts.Readback = MakeShared<FReadback>(Environment);
		OutPorts.Mutation = MakeShared<FMutation>(Environment);
		OutPorts.Settle = MakeShared<FSettle>(Environment);
		OutPorts.Comparator = MakeShared<FComparator>();
		return OutPorts.TryValidate(OutError);
	}
}

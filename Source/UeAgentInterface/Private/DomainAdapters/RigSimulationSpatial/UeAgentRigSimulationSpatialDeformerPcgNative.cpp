#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialDeformerPcgNative.h"

#include "Animation/AnimSequence.h"
#include "Components/SceneComponent.h"
#include "Dom/JsonValue.h"
#include "GeometryCache.h"
#include "GeometryCacheMeshData.h"
#include "Grid/PCGPartitionActor.h"
#include "HAL/PlatformTime.h"
#include "Misc/Crc.h"
#include "PCGComponent.h"
#include "PCGDataAsset.h"
#include "Data/PCGBasePointData.h"
#include "Engine/SkeletalMesh.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

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

		FDiagnostic NativeFact(const FString& Code, const FString& Reason, const FString& Path,
			TSharedPtr<FJsonValue> Actual, const FString& Expected, const FString& Target = FString())
		{
			auto Error = FDiagnostic::Failure(Code, Reason, TEXT("native_execution"));
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = Path;
			Error.TargetPath = Target;
			Error.ActualValuePresent = Actual.IsValid();
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValuePresent = true;
			Error.ExpectedValue = MakeShared<FJsonValueString>(Expected);
			return Error;
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
		bool Fail(FDiagnostic& OutError, FDiagnostic Diagnostic, const FString& Subject = FString(),
			const FString& NativePath = FString(), bool bCanonicalValue = false)
		{
			NativeContext(Diagnostic, NativePath, bCanonicalValue);
			OutError = MoveTemp(Diagnostic);
			if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Subject;
			return false;
		}

		bool IsCapability(const FString& CapabilityId)
		{
			return CapabilityId == TEXT("deformer.geometry.sample")
				|| CapabilityId == TEXT("deformer.training_inputs.validate")
				|| CapabilityId == TEXT("deformer.evaluation.sample");
		}

		const FUeAgentRigSimulationSpatialRequest* ProductionRequest(
			const FUeAgentRigSimulationSpatialTypedRequestRef& Request,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			const FString Field = Request->GetFamilyId() != TEXT("simulation_observation") ? TEXT("family_id")
				: Request->GetTypeId().IsEmpty() ? TEXT("type_id")
				: Request->GetRequestKindId().IsEmpty() ? TEXT("request_kind_id") : FString();
			if (!Field.IsEmpty())
			{
				const FString Actual = Field == TEXT("family_id") ? Request->GetFamilyId()
					: Field == TEXT("type_id") ? Request->GetTypeId() : Request->GetRequestKindId();
				Fail(OutError, NativeFact(TEXT("rig_simulation_spatial_deformer_pcg_typed_request_invalid"),
					TEXT("The Deformer/PCG request does not identify the required request family or type."),
					TEXT("/") + Field, MakeShared<FJsonValueString>(Actual),
					Field == TEXT("family_id") ? TEXT("simulation_observation") : TEXT("nonempty identifier")), Request->GetTypeId());
				return nullptr;
			}
			return static_cast<const FUeAgentRigSimulationSpatialRequest*>(&Request.Get());
		}

		FString ExactNumber(const double Value)
		{
			return FString::Printf(TEXT("%.17g"), Value);
		}

		FString ExactNumber(const float Value)
		{
			return FString::Printf(TEXT("%.9g"), static_cast<double>(Value));
		}

		TArray<TSharedPtr<FJsonValue>> StringValues(const TArray<FString>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> Result;
			Result.Reserve(Values.Num());
			for (const FString& Value : Values)
				Result.Add(MakeShared<FJsonValueString>(Value));
			return Result;
		}

		TSharedPtr<FJsonObject> TemporalJson(const FUeAgentTemporalPoint& Point)
		{
			const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("clock_id"), Point.GetClockId());
			Result->SetStringField(TEXT("tick"), Point.GetTick());
			Result->SetStringField(TEXT("rate_numerator"), Point.GetRateNumerator());
			Result->SetStringField(TEXT("rate_denominator"), Point.GetRateDenominator());
			Result->SetStringField(TEXT("state"), Point.GetState());
			return Result;
		}

		TSharedPtr<FJsonObject> RangeJson(const FUeAgentTemporalRange& Range)
		{
			const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("clock_id"), Range.GetClockId());
			Result->SetStringField(TEXT("state"), Range.GetState());
			if (Range.GetLowerBound().GetPoint().IsSet())
				Result->SetObjectField(TEXT("lower"),
					TemporalJson(Range.GetLowerBound().GetPoint().GetValue()));
			if (Range.GetUpperBound().GetPoint().IsSet())
				Result->SetObjectField(TEXT("upper"),
					TemporalJson(Range.GetUpperBound().GetPoint().GetValue()));
			return Result;
		}

		TSharedPtr<FJsonObject> FrameJson(const FUeAgentSpatialFrame& Frame)
		{
			const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("frame_id"), Frame.GetFrameId());
			Result->SetStringField(TEXT("basis_frame_id"), Frame.GetBasisFrameId());
			Result->SetNumberField(TEXT("dimension"), Frame.GetDimension());
			Result->SetStringField(TEXT("unit"), Frame.GetUnit());
			Result->SetStringField(TEXT("basis_evidence_id"), Frame.GetBasisEvidence());
			return Result;
		}

		FUeAgentSpatialFrame FrameAt(
			const FUeAgentSpatialFrame& Frame,
			const FUeAgentTemporalPoint& Time)
		{
			return FUeAgentSpatialFrame(Frame.GetFrameId(), Frame.GetBasisFrameId(),
				Frame.GetDimension(), Frame.GetBasis(), Frame.GetOrigin(), Frame.GetUnit(),
				Frame.GetBasisEvidence(), Time);
		}

		bool TrySeconds(
			const FUeAgentTemporalPoint& Point,
			double& OutSeconds,
			FUeAgentRigSimulationSpatialExecutionError& OutError,
			const FString& Subject, const FString& Path = TEXT("/converted_point"))
		{
			int64 Tick = 0;
			int64 Numerator = 0;
			int64 Denominator = 0;
			FString Field;
			FString Actual;
			FString Expected = TEXT("int64 decimal value");
			if (!LexTryParseString(Tick, *Point.GetTick())) { Field = TEXT("tick"); Actual = Point.GetTick(); }
			else if (!LexTryParseString(Numerator, *Point.GetRateNumerator())) { Field = TEXT("rate_numerator"); Actual = Point.GetRateNumerator(); }
			else if (!LexTryParseString(Denominator, *Point.GetRateDenominator())) { Field = TEXT("rate_denominator"); Actual = Point.GetRateDenominator(); }
			else if (Numerator <= 0) { Field = TEXT("rate_numerator"); Actual = Point.GetRateNumerator(); Expected = TEXT("positive int64"); }
			else if (Denominator <= 0) { Field = TEXT("rate_denominator"); Actual = Point.GetRateDenominator(); Expected = TEXT("positive int64"); }
			if (!Field.IsEmpty())
				return Fail(OutError, NativeFact(TEXT("rig_simulation_spatial_deformer_pcg_time_invalid"),
					TEXT("Native time cannot be converted to seconds because its tick or rate is invalid."),
					Path + TEXT("/") + Field, MakeShared<FJsonValueString>(Actual), Expected), Subject);
			OutSeconds = static_cast<double>(Tick) * static_cast<double>(Denominator)
				/ static_cast<double>(Numerator);
			return FMath::IsFinite(OutSeconds) ? true : Fail(OutError,
				NativeFact(TEXT("rig_simulation_spatial_deformer_pcg_time_invalid"),
					TEXT("Native time conversion produced non-finite seconds."), Path + TEXT("/seconds"),
					MakeShared<FJsonValueString>(ExactNumber(OutSeconds)), TEXT("finite seconds")), Subject);
		}

		bool TryNativePoint(
			IUeAgentRigSimulationSpatialNativeEnvironment& Environment,
			const FRigSimulationSpatialPoint& Input,
			const FString& NativeClockId,
			FUeAgentResourceTracker& Resources,
			FUeAgentTemporalPoint& OutPoint,
			double& OutSeconds,
			FUeAgentRigSimulationSpatialExecutionError& OutError,
			const FString& Subject)
		{
			TOptional<FUeAgentTemporalPoint> Native;
			if (!Environment.TryConvertTemporalPointToClock(Input.ToTemporalPoint(),
				NativeClockId, Resources, Native, OutError)) return false;
			if (!Native.IsSet()) return Fail(OutError,
				NativeFact(TEXT("rig_simulation_spatial_deformer_pcg_time_conversion_missing"),
					TEXT("Temporal conversion reported success without a converted point."), TEXT("/converted_point"),
					MakeShared<FJsonValueBoolean>(false), TEXT("converted temporal point")), Subject);
			OutPoint = Native.GetValue();
			return TrySeconds(OutPoint, OutSeconds, OutError, Subject);
		}

		bool TryNativeRange(
			IUeAgentRigSimulationSpatialNativeEnvironment& Environment,
			const FRigSimulationSpatialRange& Input,
			const FString& NativeClockId,
			FUeAgentResourceTracker& Resources,
			FUeAgentTemporalRange& OutRange,
			FUeAgentRigSimulationSpatialExecutionError& OutError,
			const FString& Subject)
		{
			TOptional<FUeAgentTemporalRange> Native;
			if (!Environment.TryConvertTemporalRangeToClock(Input.ToTemporalRange(),
				NativeClockId, Resources, Native, OutError)) return false;
			if (!Native.IsSet() || !Native->GetLowerBound().GetPoint().IsSet()
				|| !Native->GetUpperBound().GetPoint().IsSet())
			{
				const FString Path = !Native.IsSet() ? TEXT("/converted_range")
					: !Native->GetLowerBound().GetPoint().IsSet() ? TEXT("/converted_range/lower")
					: TEXT("/converted_range/upper");
				return Fail(OutError, NativeFact(TEXT("rig_simulation_spatial_deformer_pcg_time_conversion_missing"),
					TEXT("Temporal conversion did not provide the bounded range required for sampling."),
					Path, MakeShared<FJsonValueBoolean>(false), TEXT("present converted range bound")), Subject);
			}
			OutRange = Native.GetValue();
			double Lower = 0.0;
			double Upper = 0.0;
			if (!TrySeconds(OutRange.GetLowerBound().GetPoint().GetValue(), Lower, OutError, Subject, TEXT("/converted_range/lower"))
				|| !TrySeconds(OutRange.GetUpperBound().GetPoint().GetValue(), Upper, OutError, Subject, TEXT("/converted_range/upper"))) return false;
			if (Lower > Upper)
			{
				const auto Bounds = MakeShared<FJsonObject>();
				Bounds->SetNumberField(TEXT("lower_seconds"), Lower);
				Bounds->SetNumberField(TEXT("upper_seconds"), Upper);
				return Fail(OutError, NativeFact(TEXT("rig_simulation_spatial_deformer_pcg_time_range_invalid"),
					TEXT("The native sampling range starts after it ends."), TEXT("/converted_range"),
					MakeShared<FJsonValueObject>(Bounds), TEXT("lower_seconds <= upper_seconds")), Subject);
			}
			return true;
		}

		bool TryConvertValue(
			IUeAgentRigSimulationSpatialNativeEnvironment& Environment,
			const FString& Subject,
			EUeAgentSpatialQuantityKind Kind,
			const FUeAgentSpatialFrame& NativeFrame,
			const FUeAgentTemporalPoint& Time,
			TArray<FString> Components,
			const FString& Evidence,
			const FString& OutputFrameId,
			FUeAgentResourceTracker& Resources,
			FUeAgentSpatialValuePtr& OutValue,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			const FUeAgentSpatialValue NativeValue(Subject, Kind,
				FrameAt(NativeFrame, Time), MoveTemp(Components), Time, {Evidence});
			if (!NativeValue.IsValid()) return Fail(OutError, NativeValue.GetValidationDiagnostic(), Subject);
			if (!Environment.TryConvertSpatialValueToFrame(NativeValue, OutputFrameId,
				Resources, OutValue, OutError)) return false;
			return OutValue.IsValid() ? true : Fail(OutError,
				NativeFact(TEXT("rig_simulation_spatial_deformer_pcg_native_sample_missing"),
					TEXT("Spatial conversion reported success without a converted sample."), TEXT("/converted_sample"),
					MakeShared<FJsonValueBoolean>(false), TEXT("converted spatial sample")), Subject);
		}

		TSharedPtr<FJsonObject> SpatialValueJson(const FUeAgentSpatialValue& Value)
		{
			const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("subject"), Value.GetSubject());
			Result->SetObjectField(TEXT("frame"), FrameJson(Value.GetFrame()));
			Result->SetObjectField(TEXT("time"), TemporalJson(Value.GetObservedAt()));
			Result->SetArrayField(TEXT("components"),
				StringValues(Value.GetComponents()));
			Result->SetArrayField(TEXT("conversion_evidence_ids"),
				StringValues(Value.GetConversionEvidence()));
			return Result;
		}

		bool SelectIndices(
			const FRigSimulationSpatialSelector& Selector,
			const TArray<FString>& Refs,
			TArray<int32>& OutIndices,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			OutIndices.Reset();
			if (Selector.Mode == TEXT("all"))
			{
				for (int32 Index = 0; Index < Refs.Num(); ++Index) OutIndices.Add(Index);
			}
			else if (Selector.Mode == TEXT("refs"))
			{
				if (Selector.Refs.IsEmpty()) return Fail(OutError,
					NativeFact(TEXT("rig_simulation_spatial_selector_refs_empty"),
						TEXT("Reference selection requires at least one vertex reference."), TEXT("/vertex_selector/refs"),
						MakeShared<FJsonValueNumber>(0), TEXT("one or more vertex references")));
				for (int32 RefIndex = 0; RefIndex < Selector.Refs.Num(); ++RefIndex)
				{
					const FString& Ref = Selector.Refs[RefIndex];
					const int32 Index = Refs.Find(Ref);
					if (Index == INDEX_NONE) return Fail(OutError,
						NativeFact(TEXT("rig_simulation_spatial_selector_ref_unavailable"),
							TEXT("The selected vertex reference is absent from the sampled geometry."),
							TEXT("/vertex_selector/refs/") + FString::FromInt(RefIndex),
							MakeShared<FJsonValueString>(Ref), TEXT("reference present in sampled geometry")), Ref);
					OutIndices.AddUnique(Index);
				}
			}
			else if (Selector.Mode == TEXT("range"))
			{
				const int32 Start = Selector.Start.Get(0);
				const int32 Count = Selector.Count.Get(Refs.Num() - Start);
				if (Start < 0 || Count < 0 || Start > Refs.Num() || Count > Refs.Num() - Start)
				{
					const auto Range = MakeShared<FJsonObject>();
					Range->SetNumberField(TEXT("start"), Start);
					Range->SetNumberField(TEXT("count"), Count);
					Range->SetNumberField(TEXT("available"), Refs.Num());
					return Fail(OutError, NativeFact(TEXT("rig_simulation_spatial_selector_range_invalid"),
						TEXT("The vertex selection range is outside the sampled geometry."), TEXT("/vertex_selector"),
						MakeShared<FJsonValueObject>(Range), TEXT("0 <= start <= available; 0 <= count <= available - start")));
				}
				for (int32 Index = Start; Index < Start + Count; ++Index)
					OutIndices.Add(Index);
			}
			else return Fail(OutError, NativeFact(TEXT("rig_simulation_spatial_selector_mode_invalid"),
				TEXT("The vertex selector mode is unsupported."), TEXT("/vertex_selector/mode"),
				MakeShared<FJsonValueString>(Selector.Mode), TEXT("all, refs or range")), Selector.Mode);
			return true;
		}

		struct FGeometryVertex
		{
			FString Ref;
			FVector Position;
		};

		bool TryReadGeometry(
			UGeometryCache& Cache,
			const double Seconds,
			TArray<FGeometryVertex>& OutVertices,
			FUeAgentRigSimulationSpatialExecutionError& OutError,
			const FString& Subject)
		{
			const double Duration = static_cast<double>(Cache.CalculateDuration());
			if (Seconds < 0.0 || Seconds > Duration + UE_DOUBLE_SMALL_NUMBER)
			{
				const auto SampleTime = MakeShared<FJsonObject>();
				SampleTime->SetStringField(TEXT("seconds"), ExactNumber(Seconds));
				SampleTime->SetStringField(TEXT("duration_seconds"), ExactNumber(Duration));
				return Fail(OutError, NativeFact(TEXT("rig_simulation_spatial_geometry_sample_time_out_of_range"),
					TEXT("The sample time lies outside the geometry cache duration."), TEXT("/sample_time"),
					MakeShared<FJsonValueObject>(SampleTime), TEXT("0 <= seconds <= duration_seconds + UE_DOUBLE_SMALL_NUMBER"), Subject), Subject);
			}
			TArray<FGeometryCacheMeshData> Meshes;
			Cache.GetMeshDataAtTime(static_cast<float>(Seconds), Meshes);
			OutVertices.Reset();
			for (int32 MeshIndex = 0; MeshIndex < Meshes.Num(); ++MeshIndex)
			{
				for (int32 VertexIndex = 0;
					VertexIndex < Meshes[MeshIndex].Positions.Num(); ++VertexIndex)
				{
					OutVertices.Add({FString::Printf(TEXT("track:%d/vertex:%d"),
						MeshIndex, VertexIndex),
						FVector(Meshes[MeshIndex].Positions[VertexIndex])});
				}
			}
			if (!OutVertices.IsEmpty()) return true;
			const auto Sample = MakeShared<FJsonObject>();
			Sample->SetStringField(TEXT("seconds"), ExactNumber(Seconds));
			Sample->SetNumberField(TEXT("vertex_count"), 0);
			return Fail(OutError, NativeFact(TEXT("rig_simulation_spatial_geometry_sample_empty"),
				TEXT("The geometry cache returned no vertices at the requested sample time."), TEXT("/vertices"),
				MakeShared<FJsonValueObject>(Sample), TEXT("one or more sampled vertices"), Subject), Subject);
		}

		bool TryGeometrySample(
			const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
			const FString& PublicRef,
			UGeometryCache& Cache,
			const FString& NativeClockId,
			const FUeAgentSpatialFrame& NativeFrame,
			const FRigSimulationSpatialPoint& SampleTime,
			const FRigSimulationSpatialFrame3d& OutputFrame,
			const TOptional<FRigSimulationSpatialSelector>& Selector,
			IUeAgentRigSimulationSpatialNativeEnvironment& Environment,
			const FUeAgentOperationContext& Context,
			TSharedPtr<FJsonObject>& OutRoot,
			FUeAgentTemporalPoint& OutCapturedAt,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			double Seconds = 0.0;
			if (!TryNativePoint(Environment, SampleTime, NativeClockId,
				Context.GetResourceTracker(), OutCapturedAt, Seconds, OutError,
				PublicRef)) return false;
			TArray<FGeometryVertex> Vertices;
			if (!TryReadGeometry(Cache, Seconds, Vertices, OutError, PublicRef)) return false;
			TArray<FString> Refs;
			for (const FGeometryVertex& Vertex : Vertices) Refs.Add(Vertex.Ref);
			FRigSimulationSpatialSelector Effective;
			Effective.Mode = TEXT("all");
			if (Selector.IsSet()) Effective = Selector.GetValue();
			TArray<int32> Indices;
			if (!SelectIndices(Effective, Refs, Indices, OutError)) return false;
			TArray<TSharedPtr<FJsonValue>> VertexJson;
			for (const int32 Index : Indices)
			{
				const FGeometryVertex& Vertex = Vertices[Index];
				FUeAgentSpatialValuePtr Converted;
				const FString Evidence = Cache.GetPathName() + TEXT("#") + Vertex.Ref
					+ TEXT("@") + OutCapturedAt.GetTick();
				if (!TryConvertValue(Environment, PublicRef + TEXT("#") + Vertex.Ref,
					EUeAgentSpatialQuantityKind::Point, NativeFrame, OutCapturedAt,
					{ExactNumber(Vertex.Position.X), ExactNumber(Vertex.Position.Y),
						ExactNumber(Vertex.Position.Z)}, Evidence, OutputFrame.FrameId,
					Context.GetResourceTracker(), Converted, OutError)) return false;
				const TSharedPtr<FJsonObject> Item = SpatialValueJson(*Converted);
				Item->SetStringField(TEXT("vertex_ref"), Vertex.Ref);
				Item->SetStringField(TEXT("native_evidence_id"), Evidence);
				VertexJson.Add(MakeShared<FJsonValueObject>(Item));
			}
			const TSharedPtr<FJsonObject> Sample = MakeShared<FJsonObject>();
			Sample->SetObjectField(TEXT("time"), TemporalJson(OutCapturedAt));
			Sample->SetObjectField(TEXT("frame"),
				VertexJson.IsEmpty() ? FrameJson(NativeFrame)
					: (*VertexJson[0]->AsObject()).GetObjectField(TEXT("frame")));
			Sample->SetStringField(TEXT("time_seconds"), ExactNumber(Seconds));
			Sample->SetArrayField(TEXT("vertices"), VertexJson);
			OutRoot = MakeShared<FJsonObject>();
			OutRoot->SetStringField(TEXT("capability_id"), Contract.GetCapabilityId());
			OutRoot->SetStringField(TEXT("target_ref"), PublicRef);
			OutRoot->SetObjectField(TEXT("sample"), Sample);
			OutRoot->SetArrayField(TEXT("evidence"), StringValues({Cache.GetPathName(),
				Contract.GetReadbackId()}));
			return true;
		}

		int32 SkeletalVertexCount(const USkeletalMesh* Mesh)
		{
			const FSkeletalMeshModel* Model = Mesh ? Mesh->GetImportedModel() : nullptr;
			return Model && Model->LODModels.IsValidIndex(0)
				? Model->LODModels[0].NumVertices : INDEX_NONE;
		}

		bool TryTrainingValidation(
			const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
			const FDeformerTrainingInputsValidateInput& Input,
			IUeAgentRigSimulationSpatialNativeEnvironment& Environment,
			const FUeAgentOperationContext& Context,
			TSharedPtr<FJsonObject>& OutRoot,
			FUeAgentTemporalPoint& OutCapturedAt,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			if (Input.TrainingInputRefs.Num() != Input.TemporalRanges.Num()
				|| Input.TrainingInputRefs.Num() != Input.FrameMappings.Num())
			{
				const auto Counts = MakeShared<FJsonObject>();
				Counts->SetNumberField(TEXT("training_input_refs"), Input.TrainingInputRefs.Num());
				Counts->SetNumberField(TEXT("temporal_ranges"), Input.TemporalRanges.Num());
				Counts->SetNumberField(TEXT("frame_mappings"), Input.FrameMappings.Num());
				return Fail(OutError, NativeFact(TEXT("rig_simulation_spatial_training_input_alignment_invalid"),
					TEXT("Training inputs, temporal ranges and frame mappings have different counts."),
					FString(), MakeShared<FJsonValueObject>(Counts), TEXT("equal array lengths")));
			}
			struct FObserved
			{
				const FUeAgentRigSimulationSpatialDeformerTrainingInputBinding* Binding = nullptr;
				TOptional<FUeAgentTemporalRange> Range;
				FUeAgentSpatialValuePtr FrameProbe;
				FString Kind;
				TArray<FString> Reasons;
			};
			TArray<FObserved> Observed;
			const USkeletalMesh* Skeletal = nullptr;
			const UAnimSequence* Animation = nullptr;
			UGeometryCache* Geometry = nullptr;
			int32 SkeletalIndex = INDEX_NONE;
			int32 AnimationIndex = INDEX_NONE;
			int32 GeometryIndex = INDEX_NONE;
			for (int32 Index = 0; Index < Input.TrainingInputRefs.Num(); ++Index)
			{
				const auto* Binding = Environment.FindDeformerTrainingInputBinding(
					Input.TrainingInputRefs[Index]);
				if (!Binding) return Fail(OutError,
					NativeFact(TEXT("rig_simulation_spatial_training_input_binding_unavailable"),
						TEXT("No native training input binding exists for this reference."),
						TEXT("/training_input_refs/") + FString::FromInt(Index),
						MakeShared<FJsonValueString>(Input.TrainingInputRefs[Index]),
						TEXT("registered training input binding"), Input.TrainingInputRefs[Index]));
				if (!Binding->TryValidate(OutError)) return false;
				FObserved Item;
				Item.Binding = Binding;
				FUeAgentTemporalRange NativeRange =
					Input.TemporalRanges[Index].ToTemporalRange();
				if (!TryNativeRange(Environment, Input.TemporalRanges[Index],
					Binding->NativeClockId, Context.GetResourceTracker(), NativeRange,
					OutError, Binding->Identity)) return false;
				Item.Range = NativeRange;
				const FUeAgentTemporalPoint ProbeTime =
					Item.Range->GetLowerBound().GetPoint().GetValue();
				if (Index == 0) OutCapturedAt = ProbeTime;
				const FRigSimulationSpatialFrameMapping& Mapping = Input.FrameMappings[Index];
				if (Mapping.SourceFrame.FrameId != Binding->NativeFrame.GetFrameId())
					return Fail(OutError, NativeFact(TEXT("rig_simulation_spatial_training_frame_mapping_invalid"),
						TEXT("The training mapping source frame differs from the bound native frame."),
						TEXT("/frame_mappings/") + FString::FromInt(Index) + TEXT("/source_frame/frame_id"),
						MakeShared<FJsonValueString>(Mapping.SourceFrame.FrameId),
						Binding->NativeFrame.GetFrameId(), Input.TrainingInputRefs[Index]), Binding->Identity);
				if (Mapping.ConversionEvidenceIds.IsEmpty())
					return Fail(OutError, NativeFact(TEXT("rig_simulation_spatial_training_frame_mapping_invalid"),
						TEXT("The training frame mapping contains no conversion evidence."),
						TEXT("/frame_mappings/") + FString::FromInt(Index) + TEXT("/conversion_evidence_ids"),
						MakeShared<FJsonValueNumber>(0), TEXT("one or more conversion evidence references"),
						Input.TrainingInputRefs[Index]), Binding->Identity);
				if (!TryConvertValue(Environment, Binding->Identity,
					EUeAgentSpatialQuantityKind::Point, Binding->NativeFrame, ProbeTime,
					{TEXT("0"), TEXT("0"), TEXT("0")},
					Mapping.ConversionEvidenceIds[0], Mapping.TargetFrame.FrameId,
					Context.GetResourceTracker(), Item.FrameProbe, OutError)) return false;
				if (USkeletalMesh* Mesh = Cast<USkeletalMesh>(Binding->Input.Get()))
				{
					Skeletal = Mesh;
					Item.Kind = TEXT("skeletal_mesh");
					SkeletalIndex = Index;
				}
				else if (UAnimSequence* Sequence = Cast<UAnimSequence>(Binding->Input.Get()))
				{
					Animation = Sequence;
					Item.Kind = TEXT("animation_sequence");
					AnimationIndex = Index;
				}
				else if (UGeometryCache* Cache = Cast<UGeometryCache>(Binding->Input.Get()))
				{
					Geometry = Cache;
					Item.Kind = TEXT("geometry_cache");
					GeometryIndex = Index;
				}
				else
				{
					Item.Kind = TEXT("unsupported");
					Item.Reasons.Add(TEXT("unsupported_training_input_type"));
				}
				Observed.Add(MoveTemp(Item));
			}
			if (SkeletalIndex != INDEX_NONE && AnimationIndex != INDEX_NONE
				&& (!Skeletal->GetSkeleton() || !Animation->GetSkeleton()
					|| Skeletal->GetSkeleton() != Animation->GetSkeleton()))
			{
				Observed[SkeletalIndex].Reasons.Add(TEXT("skeleton_mismatch"));
				Observed[AnimationIndex].Reasons.Add(TEXT("skeleton_mismatch"));
			}
			if (SkeletalIndex != INDEX_NONE && GeometryIndex != INDEX_NONE)
			{
				const int32 ExpectedCount = SkeletalVertexCount(Skeletal);
				const FUeAgentTemporalRange& Range =
					Observed[GeometryIndex].Range.GetValue();
				double LowerSeconds = 0.0;
				double UpperSeconds = 0.0;
				TArray<FGeometryVertex> Lower;
				TArray<FGeometryVertex> Upper;
				if (!TrySeconds(Range.GetLowerBound().GetPoint().GetValue(), LowerSeconds,
					OutError, Input.TrainingInputRefs[GeometryIndex], TEXT("/converted_range/lower"))
					|| !TrySeconds(Range.GetUpperBound().GetPoint().GetValue(), UpperSeconds,
						OutError, Input.TrainingInputRefs[GeometryIndex], TEXT("/converted_range/upper"))
					|| !TryReadGeometry(*Geometry, LowerSeconds,
						Lower, OutError, Input.TrainingInputRefs[GeometryIndex])
					|| !TryReadGeometry(*Geometry, UpperSeconds,
						Upper, OutError, Input.TrainingInputRefs[GeometryIndex])) return false;
				if (ExpectedCount < 0 || Lower.Num() != ExpectedCount
					|| Upper.Num() != ExpectedCount)
				{
					Observed[SkeletalIndex].Reasons.Add(TEXT("topology_vertex_count_mismatch"));
					Observed[GeometryIndex].Reasons.Add(TEXT("topology_vertex_count_mismatch"));
				}
			}
			TArray<TSharedPtr<FJsonValue>> Items;
			bool bPassed = true;
			for (const FObserved& Item : Observed)
			{
				const TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
				Json->SetStringField(TEXT("input_ref"), Item.Binding->Identity);
				Json->SetStringField(TEXT("object_path"), Item.Binding->Input->GetPathName());
				Json->SetStringField(TEXT("class_path"),
					Item.Binding->Input->GetClass()->GetPathName());
				Json->SetStringField(TEXT("kind"), Item.Kind);
				Json->SetObjectField(TEXT("temporal_range"),
					RangeJson(Item.Range.GetValue()));
				Json->SetObjectField(TEXT("frame_mapping"), SpatialValueJson(*Item.FrameProbe));
				Json->SetArrayField(TEXT("reasons"), StringValues(Item.Reasons));
				Json->SetBoolField(TEXT("compatible"), Item.Reasons.IsEmpty());
				bPassed = bPassed && Item.Reasons.IsEmpty();
				Items.Add(MakeShared<FJsonValueObject>(Json));
			}
			OutRoot = MakeShared<FJsonObject>();
			OutRoot->SetStringField(TEXT("capability_id"), Contract.GetCapabilityId());
			OutRoot->SetArrayField(TEXT("inputs"), Items);
			OutRoot->SetBoolField(TEXT("validation_passed"), bPassed);
			OutRoot->SetArrayField(TEXT("evidence"), StringValues({
				TEXT("independent-training-input-object-readback"),
				TEXT("exact-requested-range-and-c4-frame-mapping")}));
			return true;
		}

		bool ConvertCanonicalValue(
			const TSharedPtr<FJsonValue>& JsonValue,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentRigSimulationSpatialExecutionError& OutError, const FString& Path = FString())
		{
			FUeAgentStateSemanticError Error;
			if (!JsonValue.IsValid()) return Fail(OutError,
				NativeFact(TEXT("rig_simulation_spatial_deformer_pcg_result_json_invalid"),
					TEXT("Native observation contains a missing JSON value."), Path,
					MakeShared<FJsonValueBoolean>(false), TEXT("present JSON value")));
			switch (JsonValue->Type)
			{
			case EJson::Null:
				return FUeAgentCanonicalValue::TryNull(Resources, OutValue, Error)
					|| Fail(OutError, Error, FString(), Path, true);
			case EJson::Boolean:
				return FUeAgentCanonicalValue::TryBoolean(JsonValue->AsBool(), Resources,
					OutValue, Error) || Fail(OutError, Error, FString(), Path, true);
			case EJson::String:
				return FUeAgentCanonicalValue::TryString(JsonValue->AsString(), Resources,
					OutValue, Error) || Fail(OutError, Error, FString(), Path, true);
			case EJson::Number:
			{
				const double Number = JsonValue->AsNumber();
				const FString Lexeme = ExactNumber(Number);
				return (FMath::IsNearlyEqual(Number, FMath::RoundToDouble(Number))
					? FUeAgentCanonicalValue::TryInteger(Lexeme, Resources, OutValue, Error)
					: FUeAgentCanonicalValue::TryDecimal(Lexeme, Resources, OutValue, Error))
					|| Fail(OutError, Error, FString(), Path, true);
			}
			case EJson::Array:
			{
				TArray<FUeAgentCanonicalValueRef> Items;
				for (const TSharedPtr<FJsonValue>& Item : JsonValue->AsArray())
				{
					FUeAgentCanonicalValuePtr Converted;
					if (!ConvertCanonicalValue(Item, Resources, Converted, OutError, Path + TEXT("/") + FString::FromInt(Items.Num()))) return false;
					Items.Add(Converted.ToSharedRef());
				}
				return FUeAgentCanonicalValue::TryArray(Items, Resources, OutValue, Error)
					|| Fail(OutError, Error, FString(), Path, true);
			}
			case EJson::Object:
			{
				TArray<FString> Keys;
				JsonValue->AsObject()->Values.GetKeys(Keys);
				Keys.Sort();
				TArray<TPair<FString, FUeAgentCanonicalValueRef>> Items;
				for (const FString& Key : Keys)
				{
					FUeAgentCanonicalValuePtr Converted;
					if (!ConvertCanonicalValue(JsonValue->AsObject()->Values.FindChecked(Key),
						Resources, Converted, OutError, Path + TEXT("/") + Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")))) return false;
					Items.Emplace(Key, Converted.ToSharedRef());
				}
				return FUeAgentCanonicalValue::TryObject(Items, Resources, OutValue, Error)
					|| Fail(OutError, Error, FString(), Path, true);
			}
			default:
				return Fail(OutError, NativeFact(TEXT("rig_simulation_spatial_deformer_pcg_result_kind_invalid"),
					TEXT("Native observation contains an unsupported JSON kind."), Path,
					MakeShared<FJsonValueNumber>(static_cast<int32>(JsonValue->Type)),
					TEXT("null, boolean, string, number, array or object")));
			}
		}

		bool TrySnapshot(
			const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
			EUeAgentRigSimulationSpatialReadPhase Phase,
			const FString& PublicRef,
			const TSharedRef<FJsonObject>& Root,
			const FUeAgentTemporalPoint& CapturedAt,
			const FUeAgentOperationContext& Context,
			TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			FUeAgentCanonicalValuePtr Value;
			if (!ConvertCanonicalValue(MakeShared<FJsonValueObject>(Root),
				Context.GetResourceTracker(), Value, OutError)) return false;
			if (!Value.IsValid()) return Fail(OutError,
				NativeFact(TEXT("rig_simulation_spatial_deformer_pcg_snapshot_value_missing"),
					TEXT("Canonical conversion reported success without a snapshot value."), TEXT("/snapshot/value"),
					MakeShared<FJsonValueBoolean>(false), TEXT("canonical snapshot value"), PublicRef));
			FString JsonText;
			const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
				TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&JsonText);
			FJsonSerializer::Serialize(Root, Writer);
			const uint64 Upper = FCrc::StrCrc32(*JsonText);
			const uint64 Lower = FCrc::StrCrc32(*(Contract.GetReadbackId() + JsonText));
			const FUeAgentStateRevision Revision(Contract.GetReadbackId(),
				TEXT("native-observation-v1"),
				static_cast<int64>(((Upper << 32) | Lower) & MAX_int64));
			const FUeAgentStateEntry Entry(
				FUeAgentCanonicalIdentity(
					TEXT("rig-simulation-spatial:") + Contract.GetCapabilityId(),
					TEXT("rig_simulation_spatial_target"), PublicRef, PublicRef,
					TEXT("result"), TEXT("native-observation-v1")),
				FUeAgentCanonicalType(Contract.GetTypedResultId(),
					EUeAgentValueKind::Object, false, TEXT("object")),
				Value.ToSharedRef(),
				FUeAgentDefaultValue(EUeAgentDefaultSourceKind::DomainNative,
					Contract.GetReadbackId(), TEXT("independent-native-readback"),
					Value.ToSharedRef()),
				FUeAgentStateDetails(TEXT("result"),
					TEXT("independent Deformer/PCG native observation"),
					{TEXT("RigSimulationSpatial"), TEXT("DeformerPcg")}, false,
					TEXT("available"), {}), Revision);
			FUeAgentStateSemanticError Error;
			return FUeAgentCanonicalSnapshot::TryCreate(
				FString::Printf(TEXT("rig-simulation-spatial:%s:%d:%s"),
					*Contract.GetCapabilityId(), static_cast<int32>(Phase),
					*Context.GetRequestId()),
				Context.GetExecutionContext().GetStatePlane(), Revision, CapturedAt,
				FUeAgentStateProvenance(TEXT("independent_readback"),
					Contract.GetReadbackId(),
					TEXT("native-state:") + Contract.GetCapabilityId()),
				{Entry}, Context.GetResourceTracker(), OutSnapshot, Error)
				? true : Fail(OutError, UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("rig_simulation_spatial_deformer_pcg_snapshot_failed"), Error, TEXT("native_execution")), Error.Source.JsonPath);
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
				if (!IsCapability(Contract.GetCapabilityId())) return Fail(OutError,
					NativeFact(TEXT("rig_simulation_spatial_family_port_unavailable"),
						TEXT("The Deformer/PCG native port does not support this capability."), TEXT("/capability_id"),
						MakeShared<FJsonValueString>(Contract.GetCapabilityId()),
						TEXT("deformer.geometry.sample, deformer.training_inputs.validate or deformer.evaluation.sample")), Contract.GetCapabilityId());
				const auto* Typed = ProductionRequest(Request, OutError);
				if (!Typed) return false;
				const FString PublicRef = Typed->GetPrimaryTargetRef();
				if (PublicRef.IsEmpty())
					return Fail(OutError, NativeFact(TEXT("rig_simulation_spatial_explicit_target_mismatch"),
						TEXT("The typed request has no primary target reference."), TEXT("/primary_target_ref"),
						MakeShared<FJsonValueString>(PublicRef), TEXT("nonempty target reference")));
				if (Target.IsSet() && Target->GetPublicRef() != PublicRef)
					return Fail(OutError, NativeFact(TEXT("rig_simulation_spatial_explicit_target_mismatch"),
						TEXT("The bound target differs from the typed request target."), TEXT("/primary_target_ref"),
						MakeShared<FJsonValueString>(Target->GetPublicRef()), PublicRef, PublicRef));
				TSharedPtr<FJsonObject> Root;
				FUeAgentTemporalPoint CapturedAt(TEXT(""), TEXT(""));
				if (Contract.GetCapabilityId() == TEXT("deformer.geometry.sample"))
				{
					const auto& Input = Typed->GetPayload().Get<FDeformerGeometrySampleInput>();
					const auto* Binding = Environment->FindDeformerGeometryBinding(
						Input.GeometryCacheRef);
					if (!Binding) return Fail(OutError,
						NativeFact(TEXT("rig_simulation_spatial_deformer_geometry_binding_unavailable"),
							TEXT("No native geometry cache binding exists for this reference."), TEXT("/geometry_cache_ref"),
							MakeShared<FJsonValueString>(Input.GeometryCacheRef), TEXT("registered geometry cache binding"), Input.GeometryCacheRef));
					if (!Binding->TryValidate(OutError)) return false;
					if (!TryGeometrySample(Contract, Input.GeometryCacheRef,
						*Binding->GeometryCache.Get(), Binding->NativeClockId,
						Binding->NativeFrame, Input.SampleTime, Input.OutputFrame,
						Input.VertexSelector, *Environment, Context, Root, CapturedAt,
						OutError)) return false;
				}
				else if (Contract.GetCapabilityId()
					== TEXT("deformer.training_inputs.validate"))
				{
					if (!TryTrainingValidation(Contract,
						Typed->GetPayload().Get<FDeformerTrainingInputsValidateInput>(),
						*Environment, Context, Root, CapturedAt, OutError)) return false;
				}
				else if (Contract.GetCapabilityId() == TEXT("deformer.evaluation.sample"))
				{
					const auto& Input = Typed->GetPayload().Get<FDeformerEvaluationSampleInput>();
					const auto* Binding = Environment->FindDeformerEvaluationBinding(
						Input.DeformerRef);
					if (!Binding) return Fail(OutError,
						NativeFact(TEXT("rig_simulation_spatial_deformer_evaluation_binding_unavailable"),
							TEXT("No native deformer evaluation binding exists for this reference."), TEXT("/deformer_ref"),
							MakeShared<FJsonValueString>(Input.DeformerRef), TEXT("registered evaluation binding"), Input.DeformerRef));
					if (!Binding->TryValidate(OutError)) return false;
					if (Binding->SubjectIdentity != Input.SubjectRef)
						return Fail(OutError, NativeFact(TEXT("rig_simulation_spatial_deformer_evaluation_binding_unavailable"),
							TEXT("The evaluation binding subject differs from the requested subject."), TEXT("/subject_ref"),
							MakeShared<FJsonValueString>(Input.SubjectRef), Binding->SubjectIdentity, Input.DeformerRef));
					if (!TryGeometrySample(Contract, Input.DeformerRef,
						*Binding->EvaluationGeometry.Get(), Binding->NativeClockId,
						Binding->NativeFrame, Input.SampleTime, Input.OutputFrame,
						Input.VertexSelector, *Environment, Context, Root, CapturedAt,
						OutError)) return false;
					Root->SetStringField(TEXT("subject_ref"), Input.SubjectRef);
					Root->SetStringField(TEXT("subject_object_path"),
						Binding->Subject->GetPathName());
				}
				if (!Root.IsValid()) return Fail(OutError,
					NativeFact(TEXT("rig_simulation_spatial_deformer_pcg_result_missing"),
						TEXT("Native observation completed without a result object."), TEXT("/result"),
						MakeShared<FJsonValueBoolean>(false), TEXT("native observation result"), PublicRef));
				if (!CapturedAt.IsValid()) return Fail(OutError, CapturedAt.GetValidationDiagnostic(), PublicRef, TEXT("/captured_at"));
				if (!TrySnapshot(Contract, Phase, PublicRef, Root.ToSharedRef(), CapturedAt,
					Context, OutSnapshot, OutError)) return false;
				OutReadback.Emplace(OutSnapshot.ToSharedRef());
				return true;
			}

		private:
			TSharedRef<IUeAgentRigSimulationSpatialNativeEnvironment> Environment;
		};

		class FReadOnlyMutation final : public IUeAgentRigSimulationSpatialMutationPort
		{
		public:
			bool TryMutate(
				const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
				const FUeAgentRigSimulationSpatialTypedRequestRef& Request,
				const TArray<FUeAgentOperationTargetBinding>& Targets,
				const FUeAgentOperationContext& Context,
				FUeAgentRigSimulationSpatialMutationResult& OutResult,
				FUeAgentRigSimulationSpatialExecutionError& OutError) override
			{
				OutResult = {};
				return Fail(OutError, NativeFact(TEXT("rig_simulation_spatial_read_only_family_mutation_forbidden"),
					TEXT("This read-only Deformer/PCG capability does not support mutation."), TEXT("/capability_id"),
					MakeShared<FJsonValueString>(Contract.GetCapabilityId()), TEXT("read-only observation")), Contract.GetCapabilityId());
			}
		};

		class FReadOnlySettle final : public IUeAgentRigSimulationSpatialSettlePort
		{
		public:
			bool TrySettle(
				const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
				const FUeAgentRigSimulationSpatialMutationResult& Mutation,
				const FUeAgentOperationContext& Context,
				FUeAgentRigSimulationSpatialExecutionError& OutError) override
			{
				return Fail(OutError, NativeFact(TEXT("rig_simulation_spatial_read_only_family_settle_forbidden"),
					TEXT("This read-only Deformer/PCG capability does not support settle."), TEXT("/capability_id"),
					MakeShared<FJsonValueString>(Contract.GetCapabilityId()), TEXT("read-only observation")), Contract.GetCapabilityId());
			}
		};

		class FReadOnlyComparator final
			: public IUeAgentRigSimulationSpatialResultComparator
		{
		public:
			bool TryCompare(
				const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
				const FUeAgentRigSimulationSpatialTypedRequestRef& Request,
				const FUeAgentCanonicalSnapshot& Before,
				const FUeAgentRigSimulationSpatialReadbackResult& After,
				const FUeAgentOperationContext& Context,
				FVerificationOutcome& OutOutcome,
				FUeAgentRigSimulationSpatialExecutionError& OutError) override
			{
				return Fail(OutError, NativeFact(TEXT("rig_simulation_spatial_read_only_family_compare_forbidden"),
					TEXT("This read-only Deformer/PCG capability does not support compare."), TEXT("/capability_id"),
					MakeShared<FJsonValueString>(Contract.GetCapabilityId()), TEXT("read-only observation")), Contract.GetCapabilityId());
			}
		};
	}

	namespace
	{
		bool DeformerBindingFailure(Contract::FDiagnostic& OutError, const TCHAR* Code,
			const TCHAR* Reason, const TCHAR* Field, const FString& Identity,
			TSharedPtr<FJsonValue> Actual, const TCHAR* Expected)
		{
			OutError = Contract::FDiagnostic::Failure(Code, Reason, TEXT("native_binding"));
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = Field;
			OutError.TargetPath = Identity;
			OutError.ActualValuePresent = Actual.IsValid();
			OutError.ActualValue = MoveTemp(Actual);
			OutError.ExpectedValuePresent = true;
			OutError.ExpectedValue = MakeShared<FJsonValueString>(Expected);
			return false;
		}

		bool ValidateDeformerFrame(const FUeAgentSpatialFrame& Frame, const FString& Identity,
			Contract::FDiagnostic& OutError)
		{
			if (Frame.IsValid()) return true;
			OutError = Frame.GetValidationDiagnostic();
			NativeContext(OutError);
			OutError.PropertyPath = TEXT("native_frame.") + OutError.PropertyPath;
			if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Identity;
			return false;
		}
	}

	bool FUeAgentRigSimulationSpatialDeformerGeometryBinding::TryValidate(Contract::FDiagnostic& OutError) const
	{
		OutError.Reset();
		if (Identity.IsEmpty()) return DeformerBindingFailure(OutError, TEXT("deformer_geometry_binding_invalid"),
			TEXT("The deformer geometry binding requires an object reference."), TEXT("identity"), Identity,
			MakeShared<FJsonValueString>(Identity), TEXT("non-empty object reference"));
		if (!GeometryCache.IsValid()) return DeformerBindingFailure(OutError, TEXT("deformer_geometry_binding_invalid"),
			TEXT("The deformer geometry cache is unavailable."), TEXT("geometry_cache"), Identity, nullptr, TEXT("valid geometry cache"));
		if (NativeClockId.IsEmpty()) return DeformerBindingFailure(OutError, TEXT("deformer_geometry_binding_invalid"),
			TEXT("The deformer geometry binding requires a native clock reference."), TEXT("native_clock_id"), Identity,
			MakeShared<FJsonValueString>(NativeClockId), TEXT("non-empty clock reference"));
		return ValidateDeformerFrame(NativeFrame, Identity, OutError);
	}

	bool FUeAgentRigSimulationSpatialDeformerGeometryBinding::IsValid() const
	{
		Contract::FDiagnostic Error;
		return TryValidate(Error);
	}

	bool FUeAgentRigSimulationSpatialDeformerTrainingInputBinding::TryValidate(Contract::FDiagnostic& OutError) const
	{
		OutError.Reset();
		if (Identity.IsEmpty()) return DeformerBindingFailure(OutError, TEXT("deformer_training_input_binding_invalid"),
			TEXT("The deformer training input binding requires an object reference."), TEXT("identity"), Identity,
			MakeShared<FJsonValueString>(Identity), TEXT("non-empty object reference"));
		if (!Input.IsValid()) return DeformerBindingFailure(OutError, TEXT("deformer_training_input_binding_invalid"),
			TEXT("The deformer training input object is unavailable."), TEXT("input"), Identity, nullptr, TEXT("valid input object"));
		if (NativeClockId.IsEmpty()) return DeformerBindingFailure(OutError, TEXT("deformer_training_input_binding_invalid"),
			TEXT("The deformer training input binding requires a native clock reference."), TEXT("native_clock_id"), Identity,
			MakeShared<FJsonValueString>(NativeClockId), TEXT("non-empty clock reference"));
		return ValidateDeformerFrame(NativeFrame, Identity, OutError);
	}

	bool FUeAgentRigSimulationSpatialDeformerTrainingInputBinding::IsValid() const
	{
		Contract::FDiagnostic Error;
		return TryValidate(Error);
	}

	bool FUeAgentRigSimulationSpatialDeformerEvaluationBinding::TryValidate(Contract::FDiagnostic& OutError) const
	{
		OutError.Reset();
		if (Identity.IsEmpty()) return DeformerBindingFailure(OutError, TEXT("deformer_evaluation_binding_invalid"),
			TEXT("The deformer evaluation binding requires an object reference."), TEXT("identity"), Identity,
			MakeShared<FJsonValueString>(Identity), TEXT("non-empty object reference"));
		if (SubjectIdentity.IsEmpty()) return DeformerBindingFailure(OutError, TEXT("deformer_evaluation_binding_invalid"),
			TEXT("The deformer evaluation binding requires a subject reference."), TEXT("subject_identity"), Identity,
			MakeShared<FJsonValueString>(SubjectIdentity), TEXT("non-empty subject reference"));
		if (!EvaluationGeometry.IsValid()) return DeformerBindingFailure(OutError, TEXT("deformer_evaluation_binding_invalid"),
			TEXT("The evaluation geometry cache is unavailable."), TEXT("evaluation_geometry"), Identity, nullptr, TEXT("valid geometry cache"));
		if (!Subject.IsValid()) return DeformerBindingFailure(OutError, TEXT("deformer_evaluation_binding_invalid"),
			TEXT("The evaluation subject object is unavailable."), TEXT("subject"), Identity, nullptr, TEXT("valid subject object"));
		if (NativeClockId.IsEmpty()) return DeformerBindingFailure(OutError, TEXT("deformer_evaluation_binding_invalid"),
			TEXT("The deformer evaluation binding requires a native clock reference."), TEXT("native_clock_id"), Identity,
			MakeShared<FJsonValueString>(NativeClockId), TEXT("non-empty clock reference"));
		return ValidateDeformerFrame(NativeFrame, Identity, OutError);
	}

	bool FUeAgentRigSimulationSpatialDeformerEvaluationBinding::IsValid() const
	{
		Contract::FDiagnostic Error;
		return TryValidate(Error);
	}

	bool TryCreateRigSimulationSpatialDeformerPcgNativePorts(
		TSharedRef<IUeAgentRigSimulationSpatialNativeEnvironment> Environment,
		FUeAgentRigSimulationSpatialNativePorts& OutPorts,
		FUeAgentRigSimulationSpatialExecutionError& OutError)
	{
		OutPorts = {};
		OutError = {};
		OutPorts.SupportedCapabilities = {
			TEXT("deformer.geometry.sample"),
			TEXT("deformer.training_inputs.validate"),
			TEXT("deformer.evaluation.sample")};
		OutPorts.Readback = MakeShared<FReadback>(Environment);
		OutPorts.Mutation = MakeShared<FReadOnlyMutation>();
		OutPorts.Settle = MakeShared<FReadOnlySettle>();
		OutPorts.Comparator = MakeShared<FReadOnlyComparator>();
		return OutPorts.TryValidate(OutError);
	}
}

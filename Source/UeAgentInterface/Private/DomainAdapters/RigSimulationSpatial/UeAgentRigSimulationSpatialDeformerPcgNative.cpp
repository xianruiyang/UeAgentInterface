#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialDeformerPcgNative.h"

#include "Animation/AnimSequence.h"
#include "Components/SceneComponent.h"
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
		bool Fail(
			FUeAgentRigSimulationSpatialExecutionError& OutError,
			FString Code,
			FString Subject = FString(),
			FString Detail = FString())
		{
			OutError = {};
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(Subject);
			OutError.Detail = MoveTemp(Detail);
			return false;
		}

		bool IsCapability(const FString& CapabilityId)
		{
			return CapabilityId == TEXT("deformer.geometry.sample")
				|| CapabilityId == TEXT("deformer.training_inputs.validate")
				|| CapabilityId == TEXT("deformer.evaluation.sample")
				|| CapabilityId == TEXT("pcg.generated_spatial_data.read")
				|| CapabilityId == TEXT("pcg.partition.spatial_state.read");
		}

		const FUeAgentRigSimulationSpatialRequest* ProductionRequest(
			const FUeAgentRigSimulationSpatialTypedRequestRef& Request,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			if (Request->GetFamilyId() != TEXT("simulation_observation")
				|| Request->GetTypeId().IsEmpty()
				|| Request->GetRequestKindId().IsEmpty())
			{
				Fail(OutError,
					TEXT("rig_simulation_spatial_deformer_pcg_typed_request_invalid"),
					Request->GetTypeId());
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

		TSharedPtr<FJsonObject> GenerationJson(
			const FRigSimulationSpatialGenerationIdentity& Identity)
		{
			const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("source_ref"), Identity.SourceRef);
			Result->SetStringField(TEXT("generation"), Identity.Generation);
			const TSharedPtr<FJsonObject> Revision = MakeShared<FJsonObject>();
			Revision->SetStringField(TEXT("source_id"), Identity.Revision.SourceId);
			Revision->SetStringField(TEXT("generation"), Identity.Revision.Generation);
			Revision->SetStringField(TEXT("sequence"),
				FString::Printf(TEXT("%lld"), Identity.Revision.Sequence));
			Result->SetObjectField(TEXT("revision"), Revision);
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
			const FString& Subject)
		{
			int64 Tick = 0;
			int64 Numerator = 0;
			int64 Denominator = 0;
			if (!LexTryParseString(Tick, *Point.GetTick())
				|| !LexTryParseString(Numerator, *Point.GetRateNumerator())
				|| !LexTryParseString(Denominator, *Point.GetRateDenominator())
				|| Numerator <= 0 || Denominator <= 0)
				return Fail(OutError,
					TEXT("rig_simulation_spatial_deformer_pcg_time_invalid"), Subject);
			OutSeconds = static_cast<double>(Tick) * static_cast<double>(Denominator)
				/ static_cast<double>(Numerator);
			return FMath::IsFinite(OutSeconds) ? true : Fail(OutError,
				TEXT("rig_simulation_spatial_deformer_pcg_time_invalid"), Subject);
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
				NativeClockId, Resources, Native, OutError) || !Native.IsSet()) return false;
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
				NativeClockId, Resources, Native, OutError) || !Native.IsSet()
				|| !Native->GetLowerBound().GetPoint().IsSet()
				|| !Native->GetUpperBound().GetPoint().IsSet()) return false;
			OutRange = Native.GetValue();
			double Lower = 0.0;
			double Upper = 0.0;
			return TrySeconds(OutRange.GetLowerBound().GetPoint().GetValue(), Lower,
				OutError, Subject) && TrySeconds(
				OutRange.GetUpperBound().GetPoint().GetValue(), Upper, OutError, Subject)
				&& Lower <= Upper;
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
			if (!NativeValue.IsValid()) return Fail(OutError,
				TEXT("rig_simulation_spatial_deformer_pcg_native_sample_invalid"), Subject);
			return Environment.TryConvertSpatialValueToFrame(NativeValue, OutputFrameId,
				Resources, OutValue, OutError) && OutValue.IsValid();
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
					TEXT("rig_simulation_spatial_selector_refs_empty"));
				for (const FString& Ref : Selector.Refs)
				{
					const int32 Index = Refs.Find(Ref);
					if (Index == INDEX_NONE) return Fail(OutError,
						TEXT("rig_simulation_spatial_selector_ref_unavailable"), Ref);
					OutIndices.AddUnique(Index);
				}
			}
			else if (Selector.Mode == TEXT("range"))
			{
				const int32 Start = Selector.Start.Get(0);
				const int32 Count = Selector.Count.Get(Refs.Num() - Start);
				if (Start < 0 || Count < 0 || Start > Refs.Num()
					|| Count > Refs.Num() - Start) return Fail(OutError,
					TEXT("rig_simulation_spatial_selector_range_invalid"));
				for (int32 Index = Start; Index < Start + Count; ++Index)
					OutIndices.Add(Index);
			}
			else return Fail(OutError,
				TEXT("rig_simulation_spatial_selector_mode_invalid"), Selector.Mode);
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
			if (Seconds < 0.0 || Seconds > static_cast<double>(Cache.CalculateDuration())
				+ UE_DOUBLE_SMALL_NUMBER) return Fail(OutError,
				TEXT("rig_simulation_spatial_geometry_sample_time_out_of_range"), Subject,
				ExactNumber(Seconds));
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
			return !OutVertices.IsEmpty() ? true : Fail(OutError,
				TEXT("rig_simulation_spatial_geometry_sample_empty"), Subject);
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
				return Fail(OutError,
					TEXT("rig_simulation_spatial_training_input_alignment_invalid"));
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
				if (!Binding || !Binding->IsValid()) return Fail(OutError,
					TEXT("rig_simulation_spatial_training_input_binding_unavailable"),
					Input.TrainingInputRefs[Index]);
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
				if (Mapping.SourceFrame.FrameId != Binding->NativeFrame.GetFrameId()
					|| Mapping.ConversionEvidenceIds.IsEmpty()) return Fail(OutError,
					TEXT("rig_simulation_spatial_training_frame_mapping_invalid"),
					Binding->Identity);
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
					OutError, Geometry->GetPathName())
					|| !TrySeconds(Range.GetUpperBound().GetPoint().GetValue(), UpperSeconds,
						OutError, Geometry->GetPathName())
					|| !TryReadGeometry(*Geometry, LowerSeconds,
						Lower, OutError, Geometry->GetPathName())
					|| !TryReadGeometry(*Geometry, UpperSeconds,
						Upper, OutError, Geometry->GetPathName())) return false;
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

		bool SameGeneration(
			const FRigSimulationSpatialGenerationIdentity& Left,
			const FRigSimulationSpatialGenerationIdentity& Right)
		{
			return Left.SourceRef == Right.SourceRef
				&& Left.Generation == Right.Generation
				&& Left.Revision == Right.Revision;
		}

		bool CheckGeneration(
			const FRigSimulationSpatialGenerationIdentity& Requested,
			const FUeAgentRigSimulationSpatialPcgGenerationBinding& Binding,
			IUeAgentRigSimulationSpatialNativeEnvironment& Environment,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			FRigSimulationSpatialRevision Current;
			return SameGeneration(Requested, Binding.Identity)
				&& Environment.TryReadRevision(Requested.SourceRef, Current, OutError)
				&& Current == Requested.Revision
				? true : Fail(OutError,
					TEXT("rig_simulation_spatial_pcg_generation_identity_mismatch"),
					Requested.SourceRef);
		}

		struct FPcgPointRecord
		{
			FString Ref;
			FPCGPoint Point;
			FString DataPath;
			FString Pin;
			TArray<FString> Tags;
		};

		bool TryPcgData(
			const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
			const FPcgGeneratedSpatialDataReadInput& Input,
			const FUeAgentRigSimulationSpatialPcgGenerationBinding& Binding,
			IUeAgentRigSimulationSpatialNativeEnvironment& Environment,
			const FUeAgentOperationContext& Context,
			TSharedPtr<FJsonObject>& OutRoot,
			FUeAgentTemporalPoint& OutCapturedAt,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			if (!CheckGeneration(Input.GenerationIdentity, Binding, Environment,
				OutError)) return false;
			OutCapturedAt = Binding.GeneratedAt;
			TArray<FPcgPointRecord> Records;
			const FPCGDataCollection& Collection = Binding.GeneratedData->Data;
			for (int32 DataIndex = 0; DataIndex < Collection.TaggedData.Num(); ++DataIndex)
			{
				const FPCGTaggedData& Tagged = Collection.TaggedData[DataIndex];
				const UPCGBasePointData* PointData = Cast<UPCGBasePointData>(Tagged.Data.Get());
				if (!PointData) continue;
				const FConstPCGPointValueRanges PointRanges(PointData);
				for (int32 PointIndex = 0; PointIndex < PointData->GetNumPoints(); ++PointIndex)
				{
					TArray<FString> Tags = Tagged.Tags.Array();
					Tags.Sort();
					FPcgPointRecord Record;
					Record.Ref = FString::Printf(TEXT("data:%d/point:%d"),
						DataIndex, PointIndex);
					Record.Point = PointRanges.GetPoint(PointIndex);
					Record.DataPath = PointData->GetPathName();
					Record.Pin = Tagged.Pin.ToString();
					Record.Tags = MoveTemp(Tags);
					Records.Add(MoveTemp(Record));
				}
			}
			TArray<FString> Refs;
			for (const FPcgPointRecord& Record : Records) Refs.Add(Record.Ref);
			TArray<int32> Indices;
			if (!SelectIndices(Input.DataSelector, Refs, Indices, OutError)) return false;
			TArray<TSharedPtr<FJsonValue>> Items;
			for (const int32 Index : Indices)
			{
				const FPcgPointRecord& Record = Records[Index];
				const FPCGPoint& Point = Record.Point;
				const FMatrix Matrix = Point.Transform.ToMatrixWithScale();
				TArray<FString> MatrixValues;
				for (int32 Row = 0; Row < 4; ++Row)
					for (int32 Column = 0; Column < 4; ++Column)
						MatrixValues.Add(ExactNumber(Matrix.M[Column][Row]));
				const FString Evidence = Binding.GeneratedData->GetPathName() + TEXT("#")
					+ Record.Ref + TEXT("@") + Binding.Identity.Generation;
				FUeAgentSpatialValuePtr Transform;
				FUeAgentSpatialValuePtr BoundsMin;
				FUeAgentSpatialValuePtr BoundsMax;
				if (!TryConvertValue(Environment, Binding.Identity.SourceRef + TEXT("#")
						+ Record.Ref, EUeAgentSpatialQuantityKind::Transform,
						Binding.NativeFrame, Binding.GeneratedAt, MoveTemp(MatrixValues),
						Evidence, Input.OutputFrame.FrameId, Context.GetResourceTracker(),
						Transform, OutError)
					|| !TryConvertValue(Environment, Binding.Identity.SourceRef + TEXT("#")
						+ Record.Ref + TEXT("/bounds_min"), EUeAgentSpatialQuantityKind::Point,
						Binding.NativeFrame, Binding.GeneratedAt,
						{ExactNumber(Point.BoundsMin.X), ExactNumber(Point.BoundsMin.Y),
							ExactNumber(Point.BoundsMin.Z)}, Evidence,
						Input.OutputFrame.FrameId, Context.GetResourceTracker(), BoundsMin,
						OutError)
					|| !TryConvertValue(Environment, Binding.Identity.SourceRef + TEXT("#")
						+ Record.Ref + TEXT("/bounds_max"), EUeAgentSpatialQuantityKind::Point,
						Binding.NativeFrame, Binding.GeneratedAt,
						{ExactNumber(Point.BoundsMax.X), ExactNumber(Point.BoundsMax.Y),
							ExactNumber(Point.BoundsMax.Z)}, Evidence,
						Input.OutputFrame.FrameId, Context.GetResourceTracker(), BoundsMax,
						OutError)) return false;
				const TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
				Json->SetStringField(TEXT("data_ref"), Record.Ref);
				Json->SetStringField(TEXT("data_path"), Record.DataPath);
				Json->SetStringField(TEXT("pin"), Record.Pin);
				Json->SetArrayField(TEXT("tags"), StringValues(Record.Tags));
				Json->SetObjectField(TEXT("transform"), SpatialValueJson(*Transform));
				Json->SetObjectField(TEXT("bounds_min"), SpatialValueJson(*BoundsMin));
				Json->SetObjectField(TEXT("bounds_max"), SpatialValueJson(*BoundsMax));
				Json->SetStringField(TEXT("density"), ExactNumber(Point.Density));
				Json->SetStringField(TEXT("seed"),
					FString::Printf(TEXT("%d"), Point.Seed));
				Json->SetStringField(TEXT("native_evidence_id"), Evidence);
				Items.Add(MakeShared<FJsonValueObject>(Json));
			}
			OutRoot = MakeShared<FJsonObject>();
			OutRoot->SetStringField(TEXT("capability_id"), Contract.GetCapabilityId());
			OutRoot->SetObjectField(TEXT("generation_identity"),
				GenerationJson(Input.GenerationIdentity));
			OutRoot->SetObjectField(TEXT("time"), TemporalJson(Binding.GeneratedAt));
			OutRoot->SetObjectField(TEXT("frame"),
				Items.IsEmpty() ? FrameJson(Binding.NativeFrame)
					: (*Items[0]->AsObject()).GetObjectField(TEXT("transform"))
						->GetObjectField(TEXT("frame")));
			OutRoot->SetArrayField(TEXT("items"), Items);
			OutRoot->SetArrayField(TEXT("evidence"), StringValues({
				Binding.GeneratedData->GetPathName(), Contract.GetReadbackId()}));
			return true;
		}

		bool TryPcgPartitions(
			const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
			const FPcgPartitionSpatialStateReadInput& Input,
			const FUeAgentRigSimulationSpatialPcgGenerationBinding& Binding,
			IUeAgentRigSimulationSpatialNativeEnvironment& Environment,
			const FUeAgentOperationContext& Context,
			TSharedPtr<FJsonObject>& OutRoot,
			FUeAgentTemporalPoint& OutCapturedAt,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			if (!CheckGeneration(Input.GenerationIdentity, Binding, Environment,
				OutError)) return false;
			OutCapturedAt = Binding.GeneratedAt;
			TArray<FString> Refs;
			for (int32 Index = 0; Index < Binding.Partitions.Num(); ++Index)
				Refs.Add(FString::Printf(TEXT("partition:%d:%s"), Index,
					*Binding.Partitions[Index]->GetPathName()));
			TArray<int32> Indices;
			if (!SelectIndices(Input.PartitionSelector, Refs, Indices, OutError)) return false;
			TArray<TSharedPtr<FJsonValue>> Items;
			for (const int32 Index : Indices)
			{
				const APCGPartitionActor* Partition = Binding.Partitions[Index].Get();
				if (!Partition) return Fail(OutError,
					TEXT("rig_simulation_spatial_pcg_partition_unavailable"), Refs[Index]);
				const FBox Bounds = Partition->GetFixedBounds();
				const FString Evidence = Partition->GetPathName() + TEXT("@")
					+ Binding.Identity.Generation;
				FUeAgentSpatialValuePtr BoundsMin;
				FUeAgentSpatialValuePtr BoundsMax;
				if (!TryConvertValue(Environment, Refs[Index] + TEXT("/bounds_min"),
						EUeAgentSpatialQuantityKind::Point, Binding.NativeFrame,
						Binding.GeneratedAt, {ExactNumber(Bounds.Min.X),
							ExactNumber(Bounds.Min.Y), ExactNumber(Bounds.Min.Z)}, Evidence,
						Input.OutputFrame.FrameId, Context.GetResourceTracker(), BoundsMin,
						OutError)
					|| !TryConvertValue(Environment, Refs[Index] + TEXT("/bounds_max"),
						EUeAgentSpatialQuantityKind::Point, Binding.NativeFrame,
						Binding.GeneratedAt, {ExactNumber(Bounds.Max.X),
							ExactNumber(Bounds.Max.Y), ExactNumber(Bounds.Max.Z)}, Evidence,
						Input.OutputFrame.FrameId, Context.GetResourceTracker(), BoundsMax,
						OutError)) return false;
				const FIntVector Grid = Partition->GetGridCoord();
				const TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
				Json->SetStringField(TEXT("partition_ref"), Refs[Index]);
				Json->SetStringField(TEXT("object_path"), Partition->GetPathName());
				Json->SetArrayField(TEXT("grid_coord"), StringValues({
					FString::Printf(TEXT("%d"), Grid.X),
					FString::Printf(TEXT("%d"), Grid.Y),
					FString::Printf(TEXT("%d"), Grid.Z)}));
				Json->SetStringField(TEXT("grid_size"),
					FString::Printf(TEXT("%u"), Partition->GetPCGGridSize()));
				Json->SetBoolField(TEXT("using_2d_grid"), Partition->IsUsing2DGrid());
				Json->SetBoolField(TEXT("runtime_generated"),
					Partition->IsRuntimeGenerated());
				Json->SetObjectField(TEXT("bounds_min"), SpatialValueJson(*BoundsMin));
				Json->SetObjectField(TEXT("bounds_max"), SpatialValueJson(*BoundsMax));
				Json->SetStringField(TEXT("native_evidence_id"), Evidence);
				Items.Add(MakeShared<FJsonValueObject>(Json));
			}
			OutRoot = MakeShared<FJsonObject>();
			OutRoot->SetStringField(TEXT("capability_id"), Contract.GetCapabilityId());
			OutRoot->SetObjectField(TEXT("generation_identity"),
				GenerationJson(Input.GenerationIdentity));
			OutRoot->SetObjectField(TEXT("time"), TemporalJson(Binding.GeneratedAt));
			OutRoot->SetObjectField(TEXT("frame"),
				Items.IsEmpty() ? FrameJson(Binding.NativeFrame)
					: (*Items[0]->AsObject()).GetObjectField(TEXT("bounds_min"))
						->GetObjectField(TEXT("frame")));
			OutRoot->SetArrayField(TEXT("items"), Items);
			OutRoot->SetArrayField(TEXT("evidence"), StringValues({
				Binding.SourceComponent->GetPathName(), Contract.GetReadbackId()}));
			return true;
		}

		bool ConvertCanonicalValue(
			const TSharedPtr<FJsonValue>& JsonValue,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			FUeAgentStateSemanticError Error;
			if (!JsonValue.IsValid()) return Fail(OutError,
				TEXT("rig_simulation_spatial_deformer_pcg_result_json_invalid"));
			switch (JsonValue->Type)
			{
			case EJson::Null:
				return FUeAgentCanonicalValue::TryNull(Resources, OutValue, Error)
					|| Fail(OutError, Error.Code, Error.Path);
			case EJson::Boolean:
				return FUeAgentCanonicalValue::TryBoolean(JsonValue->AsBool(), Resources,
					OutValue, Error) || Fail(OutError, Error.Code, Error.Path);
			case EJson::String:
				return FUeAgentCanonicalValue::TryString(JsonValue->AsString(), Resources,
					OutValue, Error) || Fail(OutError, Error.Code, Error.Path);
			case EJson::Number:
			{
				const double Number = JsonValue->AsNumber();
				const FString Lexeme = ExactNumber(Number);
				return (FMath::IsNearlyEqual(Number, FMath::RoundToDouble(Number))
					? FUeAgentCanonicalValue::TryInteger(Lexeme, Resources, OutValue, Error)
					: FUeAgentCanonicalValue::TryDecimal(Lexeme, Resources, OutValue, Error))
					|| Fail(OutError, Error.Code, Error.Path);
			}
			case EJson::Array:
			{
				TArray<FUeAgentCanonicalValueRef> Items;
				for (const TSharedPtr<FJsonValue>& Item : JsonValue->AsArray())
				{
					FUeAgentCanonicalValuePtr Converted;
					if (!ConvertCanonicalValue(Item, Resources, Converted, OutError)) return false;
					Items.Add(Converted.ToSharedRef());
				}
				return FUeAgentCanonicalValue::TryArray(Items, Resources, OutValue, Error)
					|| Fail(OutError, Error.Code, Error.Path);
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
						Resources, Converted, OutError)) return false;
					Items.Emplace(Key, Converted.ToSharedRef());
				}
				return FUeAgentCanonicalValue::TryObject(Items, Resources, OutValue, Error)
					|| Fail(OutError, Error.Code, Error.Path);
			}
			default:
				return Fail(OutError,
					TEXT("rig_simulation_spatial_deformer_pcg_result_kind_invalid"));
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
				Context.GetResourceTracker(), Value, OutError) || !Value.IsValid()) return false;
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
				? true : Fail(OutError,
					TEXT("rig_simulation_spatial_deformer_pcg_snapshot_failed"),
					Error.Path, Error.Code);
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
				TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
				FUeAgentRigSimulationSpatialExecutionError& OutError) override
			{
				OutSnapshot.Reset();
				OutError = {};
				Environment->CountFamilyDispatch();
				if (!IsCapability(Contract.GetCapabilityId())) return Fail(OutError,
					TEXT("rig_simulation_spatial_family_port_unavailable"),
					Contract.GetCapabilityId());
				const auto* Typed = ProductionRequest(Request, OutError);
				if (!Typed) return false;
				const FString PublicRef = Typed->GetPrimaryTargetRef();
				if (PublicRef.IsEmpty()
					|| (Target.IsSet() && Target->GetPublicRef() != PublicRef)) return Fail(
					OutError, TEXT("rig_simulation_spatial_explicit_target_mismatch"),
					PublicRef);
				TSharedPtr<FJsonObject> Root;
				FUeAgentTemporalPoint CapturedAt(TEXT(""), TEXT(""));
				if (Contract.GetCapabilityId() == TEXT("deformer.geometry.sample"))
				{
					const auto& Input = Typed->GetPayload().Get<FDeformerGeometrySampleInput>();
					const auto* Binding = Environment->FindDeformerGeometryBinding(
						Input.GeometryCacheRef);
					if (!Binding || !Binding->IsValid()) return Fail(OutError,
						TEXT("rig_simulation_spatial_deformer_geometry_binding_unavailable"),
						Input.GeometryCacheRef);
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
					if (!Binding || !Binding->IsValid()
						|| Binding->SubjectIdentity != Input.SubjectRef) return Fail(OutError,
						TEXT("rig_simulation_spatial_deformer_evaluation_binding_unavailable"),
						Input.DeformerRef, Input.SubjectRef);
					if (!TryGeometrySample(Contract, Input.DeformerRef,
						*Binding->EvaluationGeometry.Get(), Binding->NativeClockId,
						Binding->NativeFrame, Input.SampleTime, Input.OutputFrame,
						Input.VertexSelector, *Environment, Context, Root, CapturedAt,
						OutError)) return false;
					Root->SetStringField(TEXT("subject_ref"), Input.SubjectRef);
					Root->SetStringField(TEXT("subject_object_path"),
						Binding->Subject->GetPathName());
				}
				else if (Contract.GetCapabilityId()
					== TEXT("pcg.generated_spatial_data.read"))
				{
					const auto& Input = Typed->GetPayload().Get<FPcgGeneratedSpatialDataReadInput>();
					const auto* Binding = Environment->FindPcgGenerationBinding(
						Input.GenerationIdentity.SourceRef);
					if (!Binding || !Binding->IsValid()) return Fail(OutError,
						TEXT("rig_simulation_spatial_pcg_generation_binding_unavailable"),
						Input.GenerationIdentity.SourceRef);
					if (!TryPcgData(Contract, Input, *Binding, *Environment, Context,
						Root, CapturedAt, OutError)) return false;
				}
				else
				{
					const auto& Input = Typed->GetPayload().Get<FPcgPartitionSpatialStateReadInput>();
					const auto* Binding = Environment->FindPcgGenerationBinding(
						Input.GenerationIdentity.SourceRef);
					if (!Binding || !Binding->IsValid()) return Fail(OutError,
						TEXT("rig_simulation_spatial_pcg_generation_binding_unavailable"),
						Input.GenerationIdentity.SourceRef);
					if (!TryPcgPartitions(Contract, Input, *Binding, *Environment, Context,
						Root, CapturedAt, OutError)) return false;
				}
				return Root.IsValid() && CapturedAt.IsValid()
					&& TrySnapshot(Contract, Phase, PublicRef, Root.ToSharedRef(), CapturedAt,
						Context, OutSnapshot, OutError);
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
				return Fail(OutError,
					TEXT("rig_simulation_spatial_read_only_family_mutation_forbidden"),
					Contract.GetCapabilityId());
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
				return Fail(OutError,
					TEXT("rig_simulation_spatial_read_only_family_settle_forbidden"),
					Contract.GetCapabilityId());
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
				const FUeAgentCanonicalSnapshot& After,
				const FUeAgentOperationContext& Context,
				FVerificationOutcome& OutOutcome,
				FUeAgentRigSimulationSpatialExecutionError& OutError) override
			{
				return Fail(OutError,
					TEXT("rig_simulation_spatial_read_only_family_compare_forbidden"),
					Contract.GetCapabilityId());
			}
		};
	}

	bool FUeAgentRigSimulationSpatialDeformerGeometryBinding::IsValid() const
	{
		return !Identity.IsEmpty() && GeometryCache.IsValid()
			&& !NativeClockId.IsEmpty() && NativeFrame.IsValid();
	}

	bool FUeAgentRigSimulationSpatialDeformerTrainingInputBinding::IsValid() const
	{
		return !Identity.IsEmpty() && Input.IsValid()
			&& !NativeClockId.IsEmpty() && NativeFrame.IsValid();
	}

	bool FUeAgentRigSimulationSpatialDeformerEvaluationBinding::IsValid() const
	{
		return !Identity.IsEmpty() && !SubjectIdentity.IsEmpty()
			&& EvaluationGeometry.IsValid() && Subject.IsValid()
			&& !NativeClockId.IsEmpty() && NativeFrame.IsValid();
	}

	bool FUeAgentRigSimulationSpatialPcgGenerationBinding::IsValid() const
	{
		if (Identity.SourceRef.IsEmpty() || Identity.Generation.IsEmpty()
			|| !Identity.Revision.IsValid() || SourceComponent.IsValid() == false
			|| GeneratedData.IsValid() == false || !GeneratedAt.IsValid()
			|| !NativeFrame.IsValid()) return false;
		for (const TWeakObjectPtr<APCGPartitionActor>& Partition : Partitions)
			if (!Partition.IsValid()) return false;
		return true;
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
			TEXT("deformer.evaluation.sample"),
			TEXT("pcg.generated_spatial_data.read"),
			TEXT("pcg.partition.spatial_state.read")};
		OutPorts.Readback = MakeShared<FReadback>(Environment);
		OutPorts.Mutation = MakeShared<FReadOnlyMutation>();
		OutPorts.Settle = MakeShared<FReadOnlySettle>();
		OutPorts.Comparator = MakeShared<FReadOnlyComparator>();
		return OutPorts.IsValid() ? true : Fail(OutError,
			TEXT("rig_simulation_spatial_deformer_pcg_ports_unavailable"));
	}
}

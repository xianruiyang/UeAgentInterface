#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialRequests.h"

#include "Core/ContextResource/UeAgentContextResource.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace UE::UAI::DomainAdapters::WorldSpatial
{
	using UE::UAI::ContextResource::EUeAgentResourceDimension;
	using UE::UAI::ContextResource::FUeAgentResourceBudgetExceeded;
	using UE::UAI::ContextResource::FUeAgentResourceTracker;
	using UE::UAI::Contract::FUeAgentContractValue;

	namespace
	{
		bool Fail(
			FUeAgentWorldSpatialExecutionError& OutError,
			const FString& Code,
			const FString& SubjectId,
			const FString& Detail = FString())
		{
			OutError.Code = Code;
			OutError.SubjectId = SubjectId;
			OutError.Detail = Detail;
			return false;
		}

		bool ChargeDecode(
			FUeAgentResourceTracker& Resources,
			const FString& CapabilityId,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			FUeAgentResourceBudgetExceeded Error;
			if (!Resources.TryCharge(
					EUeAgentResourceDimension::WorkUnits,
					1,
					TEXT("world_spatial.request.decode/") + CapabilityId,
					Error))
			{
				return Fail(
					OutError,
					TEXT("world_spatial_request_decode_budget_exceeded"),
					CapabilityId,
					Error.Path);
			}
			return true;
		}

		bool TryParseRoot(
			const FUeAgentContractValue& CanonicalInput,
			TSharedPtr<FJsonObject>& OutRoot,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			OutRoot.Reset();
			const TSharedRef<TJsonReader<>> Reader =
				TJsonReaderFactory<>::Create(CanonicalInput.GetJsonValue());
			if (!FJsonSerializer::Deserialize(Reader, OutRoot)
				|| !OutRoot.IsValid())
			{
				return Fail(
					OutError,
					TEXT("world_spatial_request_json_invalid"),
					TEXT("canonical_input"));
			}
			return true;
		}

		bool RequiredString(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field,
			FString& Out,
			FUeAgentWorldSpatialExecutionError& Error)
		{
			if (!Object.IsValid()
				|| !Object->TryGetStringField(Field, Out)
				|| Out.IsEmpty())
			{
				return Fail(
					Error,
					TEXT("world_spatial_typed_field_invalid"),
					Field);
			}
			return true;
		}

		bool RequiredNumber(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field,
			double& Out,
			FUeAgentWorldSpatialExecutionError& Error)
		{
			if (!Object.IsValid() || !Object->TryGetNumberField(Field, Out))
			{
				return Fail(
					Error,
					TEXT("world_spatial_typed_field_invalid"),
					Field);
			}
			return true;
		}

		bool RequiredBool(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field,
			bool& Out,
			FUeAgentWorldSpatialExecutionError& Error)
		{
			if (!Object.IsValid() || !Object->TryGetBoolField(Field, Out))
			{
				return Fail(
					Error,
					TEXT("world_spatial_typed_field_invalid"),
					Field);
			}
			return true;
		}

		bool RequiredObject(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field,
			TSharedPtr<FJsonObject>& Out,
			FUeAgentWorldSpatialExecutionError& Error)
		{
			const TSharedPtr<FJsonObject>* Found = nullptr;
			if (!Object.IsValid()
				|| !Object->TryGetObjectField(Field, Found)
				|| !Found
				|| !Found->IsValid())
			{
				return Fail(
					Error,
					TEXT("world_spatial_typed_field_invalid"),
					Field);
			}
			Out = *Found;
			return true;
		}

		bool RequiredArray(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field,
			const TArray<TSharedPtr<FJsonValue>>*& Out,
			FUeAgentWorldSpatialExecutionError& Error)
		{
			if (!Object.IsValid()
				|| !Object->TryGetArrayField(Field, Out)
				|| !Out)
			{
				return Fail(
					Error,
					TEXT("world_spatial_typed_field_invalid"),
					Field);
			}
			return true;
		}

		bool StringArray(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field,
			TArray<FString>& Out,
			FUeAgentWorldSpatialExecutionError& Error)
		{
			Out.Reset();
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!RequiredArray(Object, Field, Values, Error))
			{
				return false;
			}
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				FString Text;
				if (!Value.IsValid()
					|| !Value->TryGetString(Text)
					|| Text.IsEmpty())
				{
					return Fail(
						Error,
						TEXT("world_spatial_typed_array_item_invalid"),
						Field);
				}
				Out.Add(MoveTemp(Text));
			}
			return true;
		}

		bool ParseRevision(
			const TSharedPtr<FJsonObject>& Object,
			FWorldSpatialRevision& Out,
			FUeAgentWorldSpatialExecutionError& Error)
		{
			double Sequence = -1.0;
			if (!RequiredString(Object, TEXT("source_id"), Out.SourceId, Error)
				|| !RequiredString(Object, TEXT("generation"), Out.Generation, Error)
				|| !RequiredNumber(Object, TEXT("sequence"), Sequence, Error))
			{
				return false;
			}
			Out.Sequence = static_cast<int64>(Sequence);
			return Out.IsValid()
				|| Fail(Error, TEXT("world_spatial_revision_invalid"), Out.SourceId);
		}

		bool ParseTemporalPoint(
			const TSharedPtr<FJsonObject>& Object,
			FWorldSpatialTemporalPoint& Out,
			FUeAgentWorldSpatialExecutionError& Error)
		{
			if (!RequiredString(Object, TEXT("clock_id"), Out.ClockId, Error)
				|| !RequiredString(Object, TEXT("tick"), Out.Tick, Error)
				|| !RequiredString(Object, TEXT("rate_numerator"), Out.RateNumerator, Error)
				|| !RequiredString(Object, TEXT("rate_denominator"), Out.RateDenominator, Error)
				|| !RequiredString(Object, TEXT("state"), Out.State, Error))
			{
				return false;
			}
			return Out.IsValid()
				|| Fail(Error, TEXT("world_spatial_temporal_point_invalid"), Out.ClockId);
		}

		bool ParseFrame(
			const TSharedPtr<FJsonObject>& Object,
			FWorldSpatialFrame& Out,
			FUeAgentWorldSpatialExecutionError& Error)
		{
			double Dimension = 0.0;
			TSharedPtr<FJsonObject> ObservedAt;
			if (!RequiredString(Object, TEXT("frame_id"), Out.FrameId, Error)
				|| !RequiredString(Object, TEXT("basis_frame_id"), Out.BasisFrameId, Error)
				|| !RequiredNumber(Object, TEXT("dimension"), Dimension, Error)
				|| !RequiredString(Object, TEXT("unit"), Out.Unit, Error)
				|| !RequiredString(Object, TEXT("basis_evidence_id"), Out.BasisEvidenceId, Error)
				|| !RequiredObject(Object, TEXT("observed_at"), ObservedAt, Error)
				|| !ParseTemporalPoint(ObservedAt, Out.ObservedAt, Error))
			{
				return false;
			}
			Out.Dimension = static_cast<int32>(Dimension);
			return Out.IsValid()
				|| Fail(Error, TEXT("world_spatial_frame_invalid"), Out.FrameId);
		}

		bool ParseSpatialValue(
			const TSharedPtr<FJsonObject>& Object,
			FWorldSpatialValue& Out,
			FUeAgentWorldSpatialExecutionError& Error)
		{
			TSharedPtr<FJsonObject> Frame;
			if (!RequiredString(Object, TEXT("subject"), Out.Subject, Error)
				|| !RequiredString(Object, TEXT("quantity"), Out.Quantity, Error)
				|| !RequiredObject(Object, TEXT("frame"), Frame, Error)
				|| !ParseFrame(Frame, Out.Frame, Error)
				|| !StringArray(Object, TEXT("components"), Out.Components, Error)
				|| !StringArray(Object, TEXT("conversion_evidence_ids"), Out.ConversionEvidenceIds, Error))
			{
				return false;
			}
			return Out.IsValid()
				|| Fail(Error, TEXT("world_spatial_value_invalid"), Out.Subject);
		}

		bool ParseExpectedRevisionEntry(
			const TSharedPtr<FJsonObject>& Object,
			FWorldSpatialExpectedRevisionEntry& Out,
			FUeAgentWorldSpatialExecutionError& Error)
		{
			TSharedPtr<FJsonObject> Revision;
			return RequiredString(Object, TEXT("target_ref"), Out.TargetRef, Error)
				&& RequiredObject(Object, TEXT("revision"), Revision, Error)
				&& ParseRevision(Revision, Out.Revision, Error)
				&& (Out.IsValid()
					|| Fail(Error, TEXT("world_spatial_expected_revision_invalid"), Out.TargetRef));
		}

		template <typename TValue, typename FParser>
		bool ObjectArray(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field,
			TArray<TValue>& Out,
			FParser&& Parser,
			FUeAgentWorldSpatialExecutionError& Error)
		{
			Out.Reset();
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!RequiredArray(Object, Field, Values, Error))
			{
				return false;
			}
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				const TSharedPtr<FJsonObject> Item =
					Value.IsValid() ? Value->AsObject() : nullptr;
				TValue Parsed;
				if (!Item.IsValid() || !Parser(Item, Parsed, Error))
				{
					return Fail(
						Error,
						Error.IsValid()
							? Error.Code
							: TEXT("world_spatial_typed_array_item_invalid"),
						Field,
						Error.Detail);
				}
				Out.Add(MoveTemp(Parsed));
			}
			return true;
		}

		bool ParseGraphViewState(
			const TSharedPtr<FJsonObject>& Object,
			FWorldSpatialGraphViewState& Out,
			FUeAgentWorldSpatialExecutionError& Error)
		{
			TSharedPtr<FJsonObject> Location;
			TSharedPtr<FJsonObject> Revision;
			return RequiredObject(Object, TEXT("location"), Location, Error)
				&& ParseSpatialValue(Location, Out.Location, Error)
				&& RequiredNumber(Object, TEXT("zoom"), Out.Zoom, Error)
				&& RequiredObject(Object, TEXT("graph_revision"), Revision, Error)
				&& ParseRevision(Revision, Out.GraphRevision, Error)
				&& (Out.IsValid()
					|| Fail(Error, TEXT("world_spatial_graph_view_state_invalid"), Out.Location.Subject));
		}

		bool ParseNodePlacement(
			const TSharedPtr<FJsonObject>& Object,
			FWorldSpatialNodePlacement& Out,
			FUeAgentWorldSpatialExecutionError& Error)
		{
			TSharedPtr<FJsonObject> Position;
			return RequiredString(Object, TEXT("node_ref"), Out.NodeRef, Error)
				&& RequiredObject(Object, TEXT("position"), Position, Error)
				&& ParseSpatialValue(Position, Out.Position, Error)
				&& (Out.IsValid()
					|| Fail(Error, TEXT("world_spatial_node_placement_invalid"), Out.NodeRef));
		}

		bool ParseLayoutRelation(
			const TSharedPtr<FJsonObject>& Object,
			FWorldSpatialLayoutRelation& Out,
			FUeAgentWorldSpatialExecutionError& Error)
		{
			if (!RequiredString(Object, TEXT("source_node_ref"), Out.SourceNodeRef, Error)
				|| !RequiredString(Object, TEXT("target_node_ref"), Out.TargetNodeRef, Error)
				|| !RequiredString(Object, TEXT("relation"), Out.Relation, Error))
			{
				return false;
			}
			if (Object->HasField(TEXT("gap")))
			{
				double Gap = 0.0;
				if (!RequiredNumber(Object, TEXT("gap"), Gap, Error))
				{
					return false;
				}
				Out.Gap = Gap;
			}
			return Out.IsValid()
				|| Fail(Error, TEXT("world_spatial_layout_relation_invalid"), Out.SourceNodeRef);
		}

		bool ParseAlignment(
			const TSharedPtr<FJsonObject>& Object,
			FWorldSpatialAlignment& Out,
			FUeAgentWorldSpatialExecutionError& Error)
		{
			TSharedPtr<FJsonObject> VerifyFrame;
			if (!RequiredString(Object, TEXT("kind"), Out.Kind, Error)
				|| !RequiredObject(Object, TEXT("verify_frame"), VerifyFrame, Error)
				|| !ParseFrame(VerifyFrame, Out.VerifyFrame, Error))
			{
				return false;
			}
			Object->TryGetStringField(TEXT("rotation_policy"), Out.RotationPolicy);
			Object->TryGetStringField(TEXT("axis"), Out.Axis);
			Object->TryGetStringField(TEXT("source_anchor"), Out.SubjectBoundsAnchor);
			Object->TryGetStringField(TEXT("target_anchor"), Out.ReferenceBoundsAnchor);
			Object->TryGetStringField(TEXT("source_face"), Out.SubjectFace);
			Object->TryGetStringField(TEXT("target_face"), Out.ReferenceFace);
			Object->TryGetNumberField(TEXT("offset_cm"), Out.OffsetCm);
			double Index = 0.0;
			if (Object->TryGetNumberField(TEXT("source_vertex_index"), Index))
			{
				Out.SubjectVertexIndex = static_cast<int32>(Index);
			}
			if (Object->TryGetNumberField(TEXT("target_vertex_index"), Index))
			{
				Out.ReferenceVertexIndex = static_cast<int32>(Index);
			}
			const TSharedPtr<FJsonObject>* AnchorObject = nullptr;
			if (Object->TryGetObjectField(TEXT("subject_anchor"), AnchorObject)
				&& AnchorObject && AnchorObject->IsValid())
			{
				FWorldSpatialValue Value;
				if (!ParseSpatialValue(*AnchorObject, Value, Error))
				{
					return false;
				}
				Out.SubjectAnchor = MoveTemp(Value);
			}
			AnchorObject = nullptr;
			if (Object->TryGetObjectField(TEXT("reference_anchor"), AnchorObject)
				&& AnchorObject && AnchorObject->IsValid())
			{
				FWorldSpatialValue Value;
				if (!ParseSpatialValue(*AnchorObject, Value, Error))
				{
					return false;
				}
				Out.ReferenceAnchor = MoveTemp(Value);
			}
			const TSharedPtr<FJsonObject>* Optional = nullptr;
			if (Object->TryGetObjectField(TEXT("subject_normal"), Optional)
				&& Optional && Optional->IsValid())
			{
				FWorldSpatialValue Value;
				if (!ParseSpatialValue(*Optional, Value, Error))
				{
					return false;
				}
				Out.SubjectNormal = MoveTemp(Value);
			}
			Optional = nullptr;
			if (Object->TryGetObjectField(TEXT("reference_normal"), Optional)
				&& Optional && Optional->IsValid())
			{
				FWorldSpatialValue Value;
				if (!ParseSpatialValue(*Optional, Value, Error))
				{
					return false;
				}
				Out.ReferenceNormal = MoveTemp(Value);
			}
			return Out.IsValid()
				|| Fail(Error, TEXT("world_spatial_alignment_invalid"), Out.Kind);
		}

		bool ParseSurfaceQuery(
			const TSharedPtr<FJsonObject>& Object,
			FWorldSpatialSurfaceQuery& Out,
			FUeAgentWorldSpatialExecutionError& Error)
		{
			TSharedPtr<FJsonObject> Origin;
			TSharedPtr<FJsonObject> Direction;
			return RequiredObject(Object, TEXT("origin"), Origin, Error)
				&& ParseSpatialValue(Origin, Out.Origin, Error)
				&& RequiredObject(Object, TEXT("direction"), Direction, Error)
				&& ParseSpatialValue(Direction, Out.Direction, Error)
				&& RequiredNumber(Object, TEXT("max_distance_cm"), Out.MaxDistanceCm, Error)
				&& RequiredString(Object, TEXT("trace_channel"), Out.TraceChannel, Error)
				&& RequiredBool(Object, TEXT("trace_complex"), Out.bTraceComplex, Error)
				&& StringArray(Object, TEXT("ignore_target_refs"), Out.IgnoreTargetRefs, Error)
				&& (Out.IsValid()
					|| Fail(Error, TEXT("world_spatial_surface_query_invalid"), Out.TraceChannel));
		}

		bool ParsePlacementPolicy(
			const TSharedPtr<FJsonObject>& Object,
			FWorldSpatialPlacementPolicy& Out,
			FUeAgentWorldSpatialExecutionError& Error)
		{
			return RequiredNumber(Object, TEXT("clearance_cm"), Out.ClearanceCm, Error)
				&& RequiredBool(Object, TEXT("align_to_surface_normal"), Out.bAlignToSurfaceNormal, Error)
				&& RequiredBool(Object, TEXT("preserve_yaw"), Out.bPreserveYaw, Error)
				&& (Out.IsValid()
					|| Fail(Error, TEXT("world_spatial_placement_policy_invalid"), TEXT("placement_policy")));
		}

		bool ParseTraceQuery(
			const TSharedPtr<FJsonObject>& Object,
			FWorldSpatialTraceQuery& Out,
			FUeAgentWorldSpatialExecutionError& Error)
		{
			return RequiredNumber(Object, TEXT("max_distance_cm"), Out.MaxDistanceCm, Error)
				&& RequiredString(Object, TEXT("trace_channel"), Out.TraceChannel, Error)
				&& RequiredBool(Object, TEXT("trace_complex"), Out.bTraceComplex, Error)
				&& StringArray(Object, TEXT("ignore_target_refs"), Out.IgnoreTargetRefs, Error)
				&& (Out.IsValid()
					|| Fail(Error, TEXT("world_spatial_trace_query_invalid"), Out.TraceChannel));
		}

		bool ParseFramePolicy(
			const TSharedPtr<FJsonObject>& Object,
			FWorldSpatialFramePolicy& Out,
			FUeAgentWorldSpatialExecutionError& Error)
		{
			if (!RequiredNumber(Object, TEXT("padding_ratio"), Out.PaddingRatio, Error)
				|| !RequiredNumber(Object, TEXT("minimum_distance_cm"), Out.MinimumDistanceCm, Error)
				|| !RequiredNumber(Object, TEXT("maximum_distance_cm"), Out.MaximumDistanceCm, Error)
				|| !RequiredBool(Object, TEXT("preserve_rotation"), Out.bPreserveRotation, Error))
			{
				return false;
			}
			Object->TryGetBoolField(TEXT("collision_aware"), Out.bCollisionAware);
			Object->TryGetBoolField(TEXT("look_at"), Out.bLookAt);
			Object->TryGetBoolField(TEXT("auto_fallback"), Out.bAutoFallback);
			return Out.IsValid()
				|| Fail(Error, TEXT("world_spatial_frame_policy_invalid"), TEXT("frame_policy"));
		}

		bool ParseViewportInteractionState(
			const TSharedPtr<FJsonObject>& Object,
			FWorldSpatialViewportInteractionState& Out,
			FUeAgentWorldSpatialExecutionError& Error)
		{
			return RequiredBool(Object, TEXT("mode_widgets"), Out.bModeWidgets, Error)
				&& RequiredBool(Object, TEXT("selection"), Out.bSelection, Error)
				&& RequiredBool(Object, TEXT("pivot"), Out.bPivot, Error)
				&& RequiredBool(Object, TEXT("editor_primitives"), Out.bEditorPrimitives, Error)
				&& RequiredBool(Object, TEXT("game_primitives"), Out.bGamePrimitives, Error)
				&& RequiredString(Object, TEXT("widget_mode"), Out.WidgetMode, Error)
				&& (Out.IsValid()
					|| Fail(Error, TEXT("world_spatial_viewport_interaction_state_invalid"), Out.WidgetMode));
		}

		bool ParseViewportCameraState(
			const TSharedPtr<FJsonObject>& Object,
			FWorldSpatialViewportCameraState& Out,
			FUeAgentWorldSpatialExecutionError& Error)
		{
			TSharedPtr<FJsonObject> Transform;
			TSharedPtr<FJsonObject> ObservedAt;
			return RequiredObject(Object, TEXT("camera_transform"), Transform, Error)
				&& ParseSpatialValue(Transform, Out.CameraTransform, Error)
				&& RequiredNumber(Object, TEXT("fov_degrees"), Out.FovDegrees, Error)
				&& RequiredString(Object, TEXT("view_mode"), Out.ViewMode, Error)
				&& RequiredObject(Object, TEXT("observed_at"), ObservedAt, Error)
				&& ParseTemporalPoint(ObservedAt, Out.ObservedAt, Error)
				&& (Out.IsValid()
					|| Fail(Error, TEXT("world_spatial_viewport_camera_state_invalid"), Out.ViewMode));
		}

		bool ParseViewportState(
			const TSharedPtr<FJsonObject>& Object,
			FWorldSpatialViewportState& Out,
			FUeAgentWorldSpatialExecutionError& Error)
		{
			TSharedPtr<FJsonObject> Camera;
			TSharedPtr<FJsonObject> Interaction;
			TSharedPtr<FJsonObject> Revision;
			return RequiredString(Object, TEXT("viewport_ref"), Out.ViewportRef, Error)
				&& RequiredObject(Object, TEXT("camera"), Camera, Error)
				&& ParseViewportCameraState(Camera, Out.Camera, Error)
				&& RequiredBool(Object, TEXT("realtime"), Out.bRealtime, Error)
				&& RequiredBool(Object, TEXT("game_view"), Out.bGameView, Error)
				&& StringArray(Object, TEXT("show_flags"), Out.ShowFlags, Error)
				&& RequiredObject(Object, TEXT("interaction"), Interaction, Error)
				&& ParseViewportInteractionState(Interaction, Out.Interaction, Error)
				&& RequiredObject(Object, TEXT("observed_revision"), Revision, Error)
				&& ParseRevision(Revision, Out.ObservedRevision, Error)
				&& (Out.IsValid()
					|| Fail(Error, TEXT("world_spatial_viewport_state_invalid"), Out.ViewportRef));
		}

		bool ParseViewportStatePatch(
			const TSharedPtr<FJsonObject>& Object,
			FWorldSpatialViewportStatePatch& Out,
			FUeAgentWorldSpatialExecutionError& Error)
		{
			const TSharedPtr<FJsonObject>* Transform = nullptr;
			if (Object->TryGetObjectField(TEXT("camera_transform"), Transform)
				&& Transform && Transform->IsValid())
			{
				FWorldSpatialValue Value;
				if (!ParseSpatialValue(*Transform, Value, Error))
				{
					return false;
				}
				Out.CameraTransform = MoveTemp(Value);
			}
			double Number = 0.0;
			if (Object->TryGetNumberField(TEXT("fov_degrees"), Number))
			{
				Out.FovDegrees = Number;
			}
			FString Text;
			if (Object->TryGetStringField(TEXT("view_mode"), Text))
			{
				Out.ViewMode = MoveTemp(Text);
			}
			bool Bool = false;
			if (Object->TryGetBoolField(TEXT("realtime"), Bool))
			{
				Out.Realtime = Bool;
			}
			if (Object->TryGetBoolField(TEXT("game_view"), Bool))
			{
				Out.GameView = Bool;
			}
			if (Object->HasField(TEXT("show_flags")))
			{
				TArray<FString> Flags;
				if (!StringArray(Object, TEXT("show_flags"), Flags, Error))
				{
					return false;
				}
				Out.ShowFlags = MoveTemp(Flags);
			}
			const TSharedPtr<FJsonObject>* Interaction = nullptr;
			if (Object->TryGetObjectField(TEXT("interaction"), Interaction)
				&& Interaction && Interaction->IsValid())
			{
				FWorldSpatialViewportInteractionState Value;
				if (!ParseViewportInteractionState(*Interaction, Value, Error))
				{
					return false;
				}
				Out.Interaction = MoveTemp(Value);
			}
			return Out.IsValid()
				|| Fail(Error, TEXT("world_spatial_viewport_state_patch_invalid"), TEXT("state_patch"));
		}

		template <typename TPayload, typename FDecode>
		bool DecodeAs(
			const TSharedPtr<FJsonObject>& Root,
			FUeAgentWorldSpatialRequestPayload& OutPayload,
			FDecode&& Decode,
			FUeAgentWorldSpatialExecutionError& Error)
		{
			TPayload Payload;
			if (!Decode(Payload))
			{
				return false;
			}
			OutPayload.Set<TPayload>(MoveTemp(Payload));
			return true;
		}

		using FPayloadDecoder = TFunction<bool(
			const TSharedPtr<FJsonObject>&,
			FUeAgentWorldSpatialRequestPayload&,
			FUeAgentWorldSpatialExecutionError&)>;

		struct FRequestSpec
		{
			const TCHAR* CapabilityId;
			const TCHAR* TypeId;
			const TCHAR* FamilyId;
			const TCHAR* RequestKindId;
			bool bOperation;
			FPayloadDecoder Decode;
		};

		const TArray<FRequestSpec>& RequestSpecs()
		{
			static const TArray<FRequestSpec> Specs = {
				{TEXT("graph.view.read"), TEXT("world_spatial.graph_view.read.request.v1"), TEXT("graph_view"), TEXT("graph_view_read"), false,
					[](const TSharedPtr<FJsonObject>& Root, FUeAgentWorldSpatialRequestPayload& Out, FUeAgentWorldSpatialExecutionError& Error)
					{
						return DecodeAs<FGraphViewReadRequestInput>(Root, Out, [&](FGraphViewReadRequestInput& P)
						{
							return !Root->HasField(TEXT("include_document_state"))
								|| Root->TryGetBoolField(
									TEXT("include_document_state"),
									P.bIncludeDocumentState)
								|| Fail(Error,
									TEXT("world_spatial_graph_document_state_flag_invalid"),
									TEXT("include_document_state"));
						}, Error);
					}},
				{TEXT("graph.view.update"), TEXT("world_spatial.graph_view.update.request.v1"), TEXT("graph_view"), TEXT("graph_view_update"), true,
					[](const TSharedPtr<FJsonObject>& Root, FUeAgentWorldSpatialRequestPayload& Out, FUeAgentWorldSpatialExecutionError& Error)
					{
						return DecodeAs<FGraphViewUpdateRequestInput>(Root, Out, [&](FGraphViewUpdateRequestInput& P)
						{
							TSharedPtr<FJsonObject> ViewState;
							if (!RequiredObject(Root, TEXT("view_state"), ViewState, Error)
								|| !ParseGraphViewState(ViewState, P.ViewState, Error))
							{
								return false;
							}
							if (Root->HasField(TEXT("update_document_state"))
								&& !Root->TryGetBoolField(
									TEXT("update_document_state"),
									P.bUpdateDocumentState))
							{
								return Fail(Error,
									TEXT("world_spatial_graph_document_state_flag_invalid"),
									TEXT("update_document_state"));
							}
							if (Root->HasField(TEXT("save_after_update"))
								&& !Root->TryGetBoolField(
									TEXT("save_after_update"), P.bSaveAfterUpdate))
							{
								return Fail(Error,
									TEXT("world_spatial_graph_document_state_flag_invalid"),
									TEXT("save_after_update"));
							}
							return !P.bSaveAfterUpdate || P.bUpdateDocumentState
								? true
								: Fail(Error,
									TEXT("world_spatial_graph_save_requires_document_state"),
									TEXT("save_after_update"));
						}, Error);
					}},
				{TEXT("graph.layout.apply"), TEXT("world_spatial.graph_layout.apply.request.v1"), TEXT("graph_layout"), TEXT("graph_layout_apply"), true,
					[](const TSharedPtr<FJsonObject>& Root, FUeAgentWorldSpatialRequestPayload& Out, FUeAgentWorldSpatialExecutionError& Error)
					{
						return DecodeAs<FGraphLayoutApplyRequestInput>(Root, Out, [&](FGraphLayoutApplyRequestInput& P)
						{
							TSharedPtr<FJsonObject> Revision;
							if (!RequiredString(Root, TEXT("graph_ref"), P.GraphRef, Error)
								|| !ObjectArray(Root, TEXT("node_placements"), P.NodePlacements, ParseNodePlacement, Error)
								|| !RequiredObject(Root, TEXT("expected_revision"), Revision, Error)
								|| !ParseRevision(Revision, P.ExpectedRevision, Error))
							{
								return false;
							}
							return !Root->HasField(TEXT("layout_relations"))
								|| ObjectArray(Root, TEXT("layout_relations"), P.LayoutRelations, ParseLayoutRelation, Error);
						}, Error);
					}},
				{TEXT("world.object_transform.read"), TEXT("world_spatial.object_transform.read.request.v1"), TEXT("object_transform"), TEXT("object_transform_read"), false,
					[](const TSharedPtr<FJsonObject>& Root, FUeAgentWorldSpatialRequestPayload& Out, FUeAgentWorldSpatialExecutionError& Error)
					{
						return DecodeAs<FObjectTransformReadRequestInput>(Root, Out, [&](FObjectTransformReadRequestInput& P)
						{
							TSharedPtr<FJsonObject> Frame;
							return RequiredObject(Root, TEXT("verify_frame"), Frame, Error)
								&& ParseFrame(Frame, P.VerifyFrame, Error);
						}, Error);
					}},
				{TEXT("world.object_transform.update"), TEXT("world_spatial.object_transform.update.request.v1"), TEXT("object_transform"), TEXT("object_transform_update"), true,
					[](const TSharedPtr<FJsonObject>& Root, FUeAgentWorldSpatialRequestPayload& Out, FUeAgentWorldSpatialExecutionError& Error)
					{
						return DecodeAs<FWorldObjectTransformUpdateRequestInput>(Root, Out, [&](FWorldObjectTransformUpdateRequestInput& P)
						{
							TSharedPtr<FJsonObject> Transform;
							TSharedPtr<FJsonObject> Revision;
							return RequiredObject(Root, TEXT("transform"), Transform, Error)
								&& ParseSpatialValue(Transform, P.Transform, Error)
								&& RequiredObject(Root, TEXT("expected_revision"), Revision, Error)
								&& ParseRevision(Revision, P.ExpectedRevision, Error);
						}, Error);
					}},
				{TEXT("world.object_align"), TEXT("world_spatial.object_transform.align.request.v1"), TEXT("object_transform"), TEXT("object_align"), true,
					[](const TSharedPtr<FJsonObject>& Root, FUeAgentWorldSpatialRequestPayload& Out, FUeAgentWorldSpatialExecutionError& Error)
					{
						return DecodeAs<FWorldObjectAlignRequestInput>(Root, Out, [&](FWorldObjectAlignRequestInput& P)
						{
							TSharedPtr<FJsonObject> Alignment;
							if (!StringArray(Root, TEXT("subject_refs"), P.SubjectRefs, Error)
								|| !RequiredObject(Root, TEXT("alignment"), Alignment, Error)
								|| !ParseAlignment(Alignment, P.Alignment, Error)
								|| !ObjectArray(Root, TEXT("expected_revisions"), P.ExpectedRevisions, ParseExpectedRevisionEntry, Error))
							{
								return false;
							}
							FString Anchor;
							if (Root->TryGetStringField(TEXT("anchor_ref"), Anchor))
							{
								P.AnchorRef = MoveTemp(Anchor);
							}
							return true;
						}, Error);
					}},
				{TEXT("world.object_snap_to_surface"), TEXT("world_spatial.object_transform.snap_to_surface.request.v1"), TEXT("object_transform"), TEXT("object_snap_to_surface"), true,
					[](const TSharedPtr<FJsonObject>& Root, FUeAgentWorldSpatialRequestPayload& Out, FUeAgentWorldSpatialExecutionError& Error)
					{
						return DecodeAs<FWorldObjectSnapToSurfaceRequestInput>(Root, Out, [&](FWorldObjectSnapToSurfaceRequestInput& P)
						{
							TSharedPtr<FJsonObject> Surface;
							TSharedPtr<FJsonObject> Policy;
							return StringArray(Root, TEXT("subject_refs"), P.SubjectRefs, Error)
								&& RequiredObject(Root, TEXT("surface_query"), Surface, Error)
								&& ParseSurfaceQuery(Surface, P.SurfaceQuery, Error)
								&& RequiredObject(Root, TEXT("placement_policy"), Policy, Error)
								&& ParsePlacementPolicy(Policy, P.PlacementPolicy, Error)
								&& ObjectArray(Root, TEXT("expected_revisions"), P.ExpectedRevisions, ParseExpectedRevisionEntry, Error);
						}, Error);
					}},
				{TEXT("mesh.transform.bake"), TEXT("world_spatial.object_transform.bake_mesh.request.v1"), TEXT("object_transform"), TEXT("mesh_transform_bake"), true,
					[](const TSharedPtr<FJsonObject>& Root, FUeAgentWorldSpatialRequestPayload& Out, FUeAgentWorldSpatialExecutionError& Error)
					{
						return DecodeAs<FMeshTransformBakeRequestInput>(Root, Out, [&](FMeshTransformBakeRequestInput& P)
						{
							TSharedPtr<FJsonObject> Transform;
							TSharedPtr<FJsonObject> Revision;
							if (!RequiredObject(Root, TEXT("transform"), Transform, Error)
								|| !ParseSpatialValue(Transform, P.Transform, Error)
								|| !RequiredObject(Root, TEXT("expected_revision"), Revision, Error)
								|| !ParseRevision(Revision, P.ExpectedRevision, Error))
							{
								return false;
							}
							bool Preserve = false;
							if (Root->TryGetBoolField(TEXT("preserve_placement"), Preserve))
							{
								P.PreservePlacement = Preserve;
							}
							return true;
						}, Error);
					}},
				{TEXT("mesh.pivot.update"), TEXT("world_spatial.object_transform.update_pivot.request.v1"), TEXT("object_transform"), TEXT("mesh_pivot_update"), true,
					[](const TSharedPtr<FJsonObject>& Root, FUeAgentWorldSpatialRequestPayload& Out, FUeAgentWorldSpatialExecutionError& Error)
					{
						return DecodeAs<FMeshPivotUpdateRequestInput>(Root, Out, [&](FMeshPivotUpdateRequestInput& P)
						{
							TSharedPtr<FJsonObject> Pivot;
							TSharedPtr<FJsonObject> Revision;
							if (!RequiredObject(Root, TEXT("pivot"), Pivot, Error)
								|| !ParseSpatialValue(Pivot, P.Pivot, Error)
								|| !RequiredObject(Root, TEXT("expected_revision"), Revision, Error)
								|| !ParseRevision(Revision, P.ExpectedRevision, Error))
							{
								return false;
							}
							bool Preserve = false;
							if (Root->TryGetBoolField(TEXT("preserve_world_geometry"), Preserve))
							{
								P.PreserveWorldGeometry = Preserve;
							}
							return true;
						}, Error);
					}},
				{TEXT("viewport.ray.deproject"), TEXT("world_spatial.viewport_projection.deproject.request.v1"), TEXT("viewport_projection"), TEXT("viewport_ray_deproject"), false,
					[](const TSharedPtr<FJsonObject>& Root, FUeAgentWorldSpatialRequestPayload& Out, FUeAgentWorldSpatialExecutionError& Error)
					{
						return DecodeAs<FViewportRayDeprojectRequestInput>(Root, Out, [&](FViewportRayDeprojectRequestInput& P)
						{
							TSharedPtr<FJsonObject> Value;
							return RequiredObject(Root, TEXT("screen_point"), Value, Error)
								&& ParseSpatialValue(Value, P.ScreenPoint, Error);
						}, Error);
					}},
				{TEXT("viewport.ray.trace"), TEXT("world_spatial.viewport_projection.trace.request.v1"), TEXT("viewport_projection"), TEXT("viewport_ray_trace"), false,
					[](const TSharedPtr<FJsonObject>& Root, FUeAgentWorldSpatialRequestPayload& Out, FUeAgentWorldSpatialExecutionError& Error)
					{
						return DecodeAs<FViewportRayTraceRequestInput>(Root, Out, [&](FViewportRayTraceRequestInput& P)
						{
							TSharedPtr<FJsonObject> Point;
							TSharedPtr<FJsonObject> Query;
							return RequiredObject(Root, TEXT("screen_point"), Point, Error)
								&& ParseSpatialValue(Point, P.ScreenPoint, Error)
								&& RequiredObject(Root, TEXT("trace_query"), Query, Error)
								&& ParseTraceQuery(Query, P.TraceQuery, Error);
						}, Error);
					}},
				{TEXT("viewport.actor.pick"), TEXT("world_spatial.viewport_projection.pick_actor.request.v1"), TEXT("viewport_projection"), TEXT("viewport_actor_pick"), false,
					[](const TSharedPtr<FJsonObject>& Root, FUeAgentWorldSpatialRequestPayload& Out, FUeAgentWorldSpatialExecutionError& Error)
					{
						return DecodeAs<FViewportActorPickRequestInput>(Root, Out, [&](FViewportActorPickRequestInput& P)
						{
							TSharedPtr<FJsonObject> Point;
							TSharedPtr<FJsonObject> Query;
							Root->TryGetBoolField(
								TEXT("allow_no_hit"), P.bAllowNoHit);
							return RequiredObject(Root, TEXT("screen_point"), Point, Error)
								&& ParseSpatialValue(Point, P.ScreenPoint, Error)
								&& RequiredObject(Root, TEXT("trace_query"), Query, Error)
								&& ParseTraceQuery(Query, P.TraceQuery, Error);
						}, Error);
					}},
				{TEXT("viewport.actor.select"), TEXT("world_spatial.viewport_projection.select_actor.request.v1"), TEXT("viewport_projection"), TEXT("viewport_actor_select"), true,
					[](const TSharedPtr<FJsonObject>& Root, FUeAgentWorldSpatialRequestPayload& Out, FUeAgentWorldSpatialExecutionError& Error)
					{
						return DecodeAs<FViewportActorSelectRequestInput>(Root, Out, [&](FViewportActorSelectRequestInput& P)
						{
							const TSharedPtr<FJsonObject>* Point = nullptr;
							if (Root->TryGetObjectField(TEXT("screen_point"), Point)
								&& Point && Point->IsValid())
							{
								FWorldSpatialValue ParsedPoint;
								if (!ParseSpatialValue(*Point, ParsedPoint, Error))
								{
									return false;
								}
								P.ScreenPoint = MoveTemp(ParsedPoint);
							}
							if (Root->HasField(TEXT("actor_refs"))
								&& !StringArray(Root, TEXT("actor_refs"), P.ActorRefs, Error))
							{
								return false;
							}
							if (!RequiredString(Root, TEXT("selection_mode"), P.SelectionMode, Error)
								|| !P.HasExactlyOneSelector())
							{
								return Error.IsValid() ? false : Fail(Error,
									TEXT("world_spatial_viewport_actor_selection_selector_invalid"),
									TEXT("exactly one of screen_point or actor_refs is required"));
							}
							const TSharedPtr<FJsonObject>* Query = nullptr;
							if (Root->TryGetObjectField(TEXT("trace_query"), Query)
								&& Query && Query->IsValid())
							{
								FWorldSpatialTraceQuery Parsed;
								if (!ParseTraceQuery(*Query, Parsed, Error))
								{
									return false;
								}
								P.TraceQuery = MoveTemp(Parsed);
							}
							if (P.TraceQuery.IsSet() && !P.ScreenPoint.IsSet())
							{
								return Fail(Error, TEXT("world_spatial_viewport_actor_selection_trace_without_screen"), TEXT("trace_query"));
							}
							return true;
						}, Error);
					}},
				{TEXT("mesh.element.select_by_ray"), TEXT("world_spatial.viewport_projection.select_mesh_element.request.v1"), TEXT("viewport_projection"), TEXT("mesh_element_select_by_ray"), true,
					[](const TSharedPtr<FJsonObject>& Root, FUeAgentWorldSpatialRequestPayload& Out, FUeAgentWorldSpatialExecutionError& Error)
					{
						return DecodeAs<FMeshElementSelectByRayRequestInput>(Root, Out, [&](FMeshElementSelectByRayRequestInput& P)
						{
							TSharedPtr<FJsonObject> Point;
							if (!RequiredObject(Root, TEXT("screen_point"), Point, Error)
								|| !ParseSpatialValue(Point, P.ScreenPoint, Error)
								|| !RequiredString(Root, TEXT("element_kind"), P.ElementKind, Error)
								|| !RequiredString(Root, TEXT("selection_mode"), P.SelectionMode, Error))
							{
								return false;
							}
							const TSharedPtr<FJsonObject>* Query = nullptr;
							if (Root->TryGetObjectField(TEXT("trace_query"), Query)
								&& Query && Query->IsValid())
							{
								FWorldSpatialTraceQuery Parsed;
								if (!ParseTraceQuery(*Query, Parsed, Error))
								{
									return false;
								}
								P.TraceQuery = MoveTemp(Parsed);
							}
							return true;
						}, Error);
					}},
				{TEXT("viewport.pixel_scale.read"), TEXT("world_spatial.viewport_projection.read_pixel_scale.request.v1"), TEXT("viewport_projection"), TEXT("viewport_pixel_scale_read"), false,
					[](const TSharedPtr<FJsonObject>& Root, FUeAgentWorldSpatialRequestPayload& Out, FUeAgentWorldSpatialExecutionError& Error)
					{
						return DecodeAs<FViewportPixelScaleReadRequestInput>(Root, Out, [&](FViewportPixelScaleReadRequestInput& P)
						{
							TSharedPtr<FJsonObject> Value;
							return RequiredObject(Root, TEXT("sample_point"), Value, Error)
								&& ParseSpatialValue(Value, P.SamplePoint, Error);
						}, Error);
					}},
				{TEXT("viewport.camera.frame"), TEXT("world_spatial.viewport_session.frame_camera.request.v1"), TEXT("viewport_session_state"), TEXT("viewport_camera_frame"), true,
					[](const TSharedPtr<FJsonObject>& Root, FUeAgentWorldSpatialRequestPayload& Out, FUeAgentWorldSpatialExecutionError& Error)
					{
						return DecodeAs<FViewportCameraFrameRequestInput>(Root, Out, [&](FViewportCameraFrameRequestInput& P)
						{
							TSharedPtr<FJsonObject> Policy;
							if (Root->HasField(TEXT("subject_refs"))
								&& !StringArray(Root, TEXT("subject_refs"), P.SubjectRefs, Error))
							{
								return false;
							}
							FString Folder;
							if (Root->TryGetStringField(TEXT("folder_path"), Folder))
							{
								P.FolderPath = MoveTemp(Folder);
							}
							Root->TryGetBoolField(TEXT("include_child_folders"), P.bIncludeChildFolders);
							Root->TryGetBoolField(TEXT("use_current_selection"), P.bUseCurrentSelection);
							return RequiredObject(Root, TEXT("frame_policy"), Policy, Error)
								&& ParseFramePolicy(Policy, P.FramePolicy, Error)
								&& (P.HasExactlyOneSelector()
									|| Fail(Error, TEXT("world_spatial_viewport_frame_selector_invalid"),
										TEXT("exactly one of subject_refs, folder_path or use_current_selection is required")));
						}, Error);
					}},
				{TEXT("viewport.state.read"), TEXT("world_spatial.viewport_session.read_state.request.v1"), TEXT("viewport_session_state"), TEXT("viewport_state_read"), false,
					[](const TSharedPtr<FJsonObject>& Root, FUeAgentWorldSpatialRequestPayload& Out, FUeAgentWorldSpatialExecutionError& Error)
					{
						return DecodeAs<FViewportStateReadRequestInput>(Root, Out, [](FViewportStateReadRequestInput&) { return true; }, Error);
					}},
				{TEXT("viewport.state.restore"), TEXT("world_spatial.viewport_session.restore_state.request.v1"), TEXT("viewport_session_state"), TEXT("viewport_state_restore"), true,
					[](const TSharedPtr<FJsonObject>& Root, FUeAgentWorldSpatialRequestPayload& Out, FUeAgentWorldSpatialExecutionError& Error)
					{
						return DecodeAs<FViewportStateRestoreRequestInput>(Root, Out, [&](FViewportStateRestoreRequestInput& P)
						{
							TSharedPtr<FJsonObject> Snapshot;
							return RequiredObject(Root, TEXT("snapshot"), Snapshot, Error)
								&& ParseViewportState(Snapshot, P.Snapshot, Error);
						}, Error);
					}},
				{TEXT("viewport.warning.list"), TEXT("world_spatial.viewport_session.list_warnings.request.v1"), TEXT("viewport_session_state"), TEXT("viewport_warning_list"), false,
					[](const TSharedPtr<FJsonObject>& Root, FUeAgentWorldSpatialRequestPayload& Out, FUeAgentWorldSpatialExecutionError& Error)
					{
						return DecodeAs<FViewportWarningListRequestInput>(Root, Out, [&](FViewportWarningListRequestInput& P)
						{
							FString Ref;
							if (Root->TryGetStringField(TEXT("viewport_ref"), Ref))
							{
								P.ViewportRef = MoveTemp(Ref);
							}
							Root->TryGetBoolField(TEXT("include_suppressed"), P.bIncludeSuppressed);
							return true;
						}, Error);
					}},
				{TEXT("viewport.view_mode.list"), TEXT("world_spatial.viewport_session.list_view_modes.request.v1"), TEXT("viewport_session_state"), TEXT("viewport_view_mode_list"), false,
					[](const TSharedPtr<FJsonObject>& Root, FUeAgentWorldSpatialRequestPayload& Out, FUeAgentWorldSpatialExecutionError& Error)
					{
						return DecodeAs<FViewportViewModeListRequestInput>(Root, Out, [](FViewportViewModeListRequestInput&) { return true; }, Error);
					}},
				{TEXT("viewport.instance.list"), TEXT("world_spatial.viewport_session.list_instances.request.v1"), TEXT("viewport_session_state"), TEXT("viewport_instance_list"), false,
					[](const TSharedPtr<FJsonObject>& Root, FUeAgentWorldSpatialRequestPayload& Out, FUeAgentWorldSpatialExecutionError& Error)
					{
						return DecodeAs<FViewportInstanceListRequestInput>(Root, Out, [](FViewportInstanceListRequestInput&) { return true; }, Error);
					}},
				{TEXT("viewport.state.update"), TEXT("world_spatial.viewport_session.update_state.request.v1"), TEXT("viewport_session_state"), TEXT("viewport_state_update"), true,
					[](const TSharedPtr<FJsonObject>& Root, FUeAgentWorldSpatialRequestPayload& Out, FUeAgentWorldSpatialExecutionError& Error)
					{
						return DecodeAs<FViewportStateUpdateRequestInput>(Root, Out, [&](FViewportStateUpdateRequestInput& P)
						{
							TSharedPtr<FJsonObject> Patch;
							return RequiredObject(Root, TEXT("state_patch"), Patch, Error)
								&& ParseViewportStatePatch(Patch, P.StatePatch, Error);
						}, Error);
					}},
			};
			return Specs;
		}

		class FWorldSpatialRequestDecoder final
			: public IUeAgentWorldSpatialRequestDecoder
		{
		public:
			bool TryDecode(
				const FUeAgentWorldSpatialExecutionChainContract& Contract,
				const FUeAgentContractValue& CanonicalInput,
				FUeAgentResourceTracker& Resources,
				FUeAgentWorldSpatialTypedRequestPtr& OutRequest,
				FUeAgentWorldSpatialExecutionError& OutError) override
			{
				OutRequest.Reset();
				OutError = {};
				const FRequestSpec* Spec = RequestSpecs().FindByPredicate(
					[&Contract](const FRequestSpec& Candidate)
					{
						return Contract.GetCapabilityId() == Candidate.CapabilityId;
					});
				if (!Spec
					|| Contract.GetTypedRequestId() != Spec->TypeId
					|| Contract.GetFamilyId() != Spec->FamilyId
					|| Contract.RequiresNativeMutation() != Spec->bOperation)
				{
					return Fail(
						OutError,
						TEXT("world_spatial_request_contract_mismatch"),
						Contract.GetCapabilityId());
				}
				if (!ChargeDecode(Resources, Contract.GetCapabilityId(), OutError))
				{
					return false;
				}
				TSharedPtr<FJsonObject> Root;
				if (!TryParseRoot(CanonicalInput, Root, OutError))
				{
					return false;
				}
				FUeAgentWorldSpatialRequestPayload Payload;
				if (!Spec->Decode(Root, Payload, OutError))
				{
					if (!OutError.IsValid())
					{
						Fail(
							OutError,
							TEXT("world_spatial_request_decode_failed"),
							Contract.GetCapabilityId());
					}
					return false;
				}
				OutRequest = MakeShared<FUeAgentWorldSpatialRequest>(
					Spec->TypeId,
					Spec->FamilyId,
					Spec->RequestKindId,
					Spec->bOperation,
					MoveTemp(Payload));
				return OutRequest.IsValid();
			}
		};
	}

	bool FWorldSpatialRevision::IsValid() const
	{
		return !SourceId.IsEmpty() && !Generation.IsEmpty() && Sequence >= 0;
	}

	bool FWorldSpatialTemporalPoint::IsValid() const
	{
		return !ClockId.IsEmpty()
			&& !Tick.IsEmpty()
			&& !RateNumerator.IsEmpty()
			&& !RateDenominator.IsEmpty()
			&& (State == TEXT("captured") || State == TEXT("frozen"));
	}

	bool FWorldSpatialFrame::IsValid() const
	{
		return !FrameId.IsEmpty()
			&& !BasisFrameId.IsEmpty()
			&& (Dimension == 2 || Dimension == 3)
			&& !Unit.IsEmpty()
			&& !BasisEvidenceId.IsEmpty()
			&& ObservedAt.IsValid();
	}

	bool FWorldSpatialValue::IsValid() const
	{
		const int32 ExpectedComponents = Quantity == TEXT("transform")
			? 10
			: Frame.Dimension;
		return !Subject.IsEmpty()
			&& (Quantity == TEXT("point")
				|| Quantity == TEXT("direction")
				|| Quantity == TEXT("normal")
				|| Quantity == TEXT("transform"))
			&& Frame.IsValid()
			&& Components.Num() == ExpectedComponents
			&& !Components.ContainsByPredicate(
				[](const FString& Value) { return Value.IsEmpty(); });
	}

	bool FWorldSpatialExpectedRevisionEntry::IsValid() const
	{
		return !TargetRef.IsEmpty() && Revision.IsValid();
	}

	bool FWorldSpatialGraphViewState::IsValid() const
	{
		return Location.IsValid()
			&& Location.Frame.Dimension == 2
			&& Zoom > 0.0
			&& GraphRevision.IsValid();
	}

	bool FWorldSpatialNodePlacement::IsValid() const
	{
		return !NodeRef.IsEmpty()
			&& Position.IsValid()
			&& Position.Frame.Dimension == 2;
	}

	bool FWorldSpatialLayoutRelation::IsValid() const
	{
		return !SourceNodeRef.IsEmpty()
			&& !TargetNodeRef.IsEmpty()
			&& !Relation.IsEmpty()
			&& (!Gap.IsSet() || Gap.GetValue() >= 0.0);
	}

	bool FWorldSpatialAlignment::IsValid() const
	{
		if (!VerifyFrame.IsValid() || VerifyFrame.Dimension != 3
			|| (RotationPolicy != TEXT("preserve")
				&& RotationPolicy != TEXT("align_normals")
				&& RotationPolicy != TEXT("align_frames"))
			|| (SubjectNormal.IsSet() && !SubjectNormal->IsValid())
			|| (ReferenceNormal.IsSet() && !ReferenceNormal->IsValid()))
		{
			return false;
		}
		if (Kind == TEXT("anchors"))
		{
			return SubjectAnchor.IsSet() && SubjectAnchor->IsValid()
				&& ReferenceAnchor.IsSet() && ReferenceAnchor->IsValid();
		}
		if (Kind == TEXT("bounds"))
		{
			return (Axis == TEXT("x") || Axis == TEXT("y") || Axis == TEXT("z"))
				&& (SubjectBoundsAnchor == TEXT("min") || SubjectBoundsAnchor == TEXT("center") || SubjectBoundsAnchor == TEXT("max"))
				&& (ReferenceBoundsAnchor == TEXT("min") || ReferenceBoundsAnchor == TEXT("center") || ReferenceBoundsAnchor == TEXT("max"));
		}
		if (Kind == TEXT("face"))
		{
			return !SubjectFace.IsEmpty() && !ReferenceFace.IsEmpty()
				&& (Axis.IsEmpty() || Axis == TEXT("x") || Axis == TEXT("y") || Axis == TEXT("z"));
		}
		if (Kind == TEXT("vertex"))
		{
			return (!SubjectVertexIndex.IsSet() || SubjectVertexIndex.GetValue() >= 0)
				&& (!ReferenceVertexIndex.IsSet() || ReferenceVertexIndex.GetValue() >= 0);
		}
		return false;
	}

	bool FWorldSpatialSurfaceQuery::IsValid() const
	{
		return Origin.IsValid()
			&& Direction.IsValid()
			&& Origin.Frame.Dimension == 3
			&& Direction.Frame.Dimension == 3
			&& MaxDistanceCm > 0.0
			&& !TraceChannel.IsEmpty();
	}

	bool FWorldSpatialPlacementPolicy::IsValid() const
	{
		return ClearanceCm >= 0.0;
	}

	bool FWorldSpatialTraceQuery::IsValid() const
	{
		return MaxDistanceCm > 0.0 && !TraceChannel.IsEmpty();
	}

	bool FWorldSpatialFramePolicy::IsValid() const
	{
		return PaddingRatio >= 0.0
			&& MinimumDistanceCm >= 0.0
			&& MaximumDistanceCm > 0.0
			&& MaximumDistanceCm >= MinimumDistanceCm;
	}

	bool FWorldSpatialViewportInteractionState::IsValid() const
	{
		return !WidgetMode.IsEmpty();
	}

	bool FWorldSpatialViewportCameraState::IsValid() const
	{
		return CameraTransform.IsValid()
			&& CameraTransform.Frame.Dimension == 3
			&& FovDegrees > 0.0
			&& FovDegrees <= 179.0
			&& !ViewMode.IsEmpty()
			&& ObservedAt.IsValid();
	}

	bool FWorldSpatialViewportState::IsValid() const
	{
		return !ViewportRef.IsEmpty()
			&& Camera.IsValid()
			&& Interaction.IsValid()
			&& ObservedRevision.IsValid();
	}

	bool FWorldSpatialViewportStatePatch::IsValid() const
	{
		return (!CameraTransform.IsSet() || CameraTransform->IsValid())
			&& (!FovDegrees.IsSet()
				|| (FovDegrees.GetValue() > 0.0 && FovDegrees.GetValue() <= 179.0))
			&& (!ViewMode.IsSet() || !ViewMode->IsEmpty())
			&& (!Interaction.IsSet() || Interaction->IsValid());
	}

	bool FViewportActorSelectRequestInput::HasExactlyOneSelector() const
	{
		return static_cast<int32>(ScreenPoint.IsSet())
			+ static_cast<int32>(ActorRefs.Num() > 0) == 1;
	}

	bool FViewportCameraFrameRequestInput::HasExactlyOneSelector() const
	{
		return static_cast<int32>(SubjectRefs.Num() > 0)
			+ static_cast<int32>(FolderPath.IsSet() && !FolderPath->IsEmpty())
			+ static_cast<int32>(bUseCurrentSelection) == 1;
	}

	namespace
	{
		using UE::UAI::StateSemantic::FUeAgentCanonicalValue;
		using UE::UAI::StateSemantic::FUeAgentCanonicalValuePtr;
		using UE::UAI::StateSemantic::FUeAgentCanonicalValueRef;
		using UE::UAI::StateSemantic::FUeAgentStateSemanticError;

		class FTypedIntentBuilder final
		{
		public:
			FTypedIntentBuilder(
				FUeAgentResourceTracker& InResources,
				FUeAgentWorldSpatialExecutionError& InError,
				const FString& InRequestKind)
				: Resources(InResources)
				, Error(InError)
				, RequestKind(InRequestKind)
			{
			}

			bool String(const FString& Value, FUeAgentCanonicalValuePtr& Out)
			{
				FUeAgentStateSemanticError StateError;
				if (!FUeAgentCanonicalValue::TryString(
						Value,
						Resources,
						Out,
						StateError))
				{
					return FailBuild(StateError.Code);
				}
				return true;
			}

			bool Boolean(const bool Value, FUeAgentCanonicalValuePtr& Out)
			{
				FUeAgentStateSemanticError StateError;
				if (!FUeAgentCanonicalValue::TryBoolean(
						Value,
						Resources,
						Out,
						StateError))
				{
					return FailBuild(StateError.Code);
				}
				return true;
			}

			bool Decimal(const double Value, FUeAgentCanonicalValuePtr& Out)
			{
				FUeAgentStateSemanticError StateError;
				if (!FUeAgentCanonicalValue::TryDecimal(
						FString::SanitizeFloat(Value, 0),
						Resources,
						Out,
						StateError))
				{
					return FailBuild(StateError.Code);
				}
				return true;
			}

			bool Integer(const int64 Value, FUeAgentCanonicalValuePtr& Out)
			{
				FUeAgentStateSemanticError StateError;
				if (!FUeAgentCanonicalValue::TryInteger(
						FString::Printf(TEXT("%lld"), Value),
						Resources,
						Out,
						StateError))
				{
					return FailBuild(StateError.Code);
				}
				return true;
			}

			bool Object(
				const TArray<TPair<FString, FUeAgentCanonicalValueRef>>& Items,
				FUeAgentCanonicalValuePtr& Out)
			{
				FUeAgentStateSemanticError StateError;
				if (!FUeAgentCanonicalValue::TryObject(
						Items,
						Resources,
						Out,
						StateError))
				{
					return FailBuild(StateError.Code);
				}
				return true;
			}

			bool Array(
				const TArray<FUeAgentCanonicalValueRef>& Items,
				FUeAgentCanonicalValuePtr& Out)
			{
				FUeAgentStateSemanticError StateError;
				if (!FUeAgentCanonicalValue::TryArray(
						Items,
						Resources,
						Out,
						StateError))
				{
					return FailBuild(StateError.Code);
				}
				return true;
			}

			bool StringArray(
				const TArray<FString>& Values,
				FUeAgentCanonicalValuePtr& Out)
			{
				TArray<FUeAgentCanonicalValueRef> Items;
				for (const FString& Value : Values)
				{
					FUeAgentCanonicalValuePtr Item;
					if (!String(Value, Item))
					{
						return false;
					}
					Items.Add(Item.ToSharedRef());
				}
				return Array(Items, Out);
			}

			bool Temporal(
				const FWorldSpatialTemporalPoint& Value,
				FUeAgentCanonicalValuePtr& Out)
			{
				TArray<TPair<FString, FUeAgentCanonicalValueRef>> Items;
				return AddString(Items, TEXT("clock_id"), Value.ClockId)
					&& AddString(Items, TEXT("tick"), Value.Tick)
					&& AddString(Items, TEXT("rate_numerator"), Value.RateNumerator)
					&& AddString(Items, TEXT("rate_denominator"), Value.RateDenominator)
					&& AddString(Items, TEXT("state"), Value.State)
					&& Object(Items, Out);
			}

			bool Frame(
				const FWorldSpatialFrame& Value,
				FUeAgentCanonicalValuePtr& Out)
			{
				TArray<TPair<FString, FUeAgentCanonicalValueRef>> Items;
				FUeAgentCanonicalValuePtr ObservedAt;
				return Temporal(Value.ObservedAt, ObservedAt)
					&& AddString(Items, TEXT("frame_id"), Value.FrameId)
					&& AddString(Items, TEXT("basis_frame_id"), Value.BasisFrameId)
					&& AddInteger(Items, TEXT("dimension"), Value.Dimension)
					&& AddString(Items, TEXT("unit"), Value.Unit)
					&& AddString(Items, TEXT("basis_evidence_id"), Value.BasisEvidenceId)
					&& Add(Items, TEXT("observed_at"), ObservedAt)
					&& Object(Items, Out);
			}

			bool SpatialValue(
				const FWorldSpatialValue& Value,
				FUeAgentCanonicalValuePtr& Out)
			{
				TArray<TPair<FString, FUeAgentCanonicalValueRef>> Items;
				FUeAgentCanonicalValuePtr FrameValue;
				FUeAgentCanonicalValuePtr Components;
				FUeAgentCanonicalValuePtr Evidence;
				return Frame(Value.Frame, FrameValue)
					&& StringArray(Value.Components, Components)
					&& StringArray(Value.ConversionEvidenceIds, Evidence)
					&& AddString(Items, TEXT("subject"), Value.Subject)
					&& AddString(Items, TEXT("quantity"), Value.Quantity)
					&& Add(Items, TEXT("frame"), FrameValue)
					&& Add(Items, TEXT("components"), Components)
					&& Add(Items, TEXT("conversion_evidence_ids"), Evidence)
					&& Object(Items, Out);
			}

			bool Revision(
				const FWorldSpatialRevision& Value,
				FUeAgentCanonicalValuePtr& Out)
			{
				TArray<TPair<FString, FUeAgentCanonicalValueRef>> Items;
				return AddString(Items, TEXT("source_id"), Value.SourceId)
					&& AddString(Items, TEXT("generation"), Value.Generation)
					&& AddInteger(Items, TEXT("sequence"), Value.Sequence)
					&& Object(Items, Out);
			}

			bool TraceQuery(
				const FWorldSpatialTraceQuery& Value,
				FUeAgentCanonicalValuePtr& Out)
			{
				TArray<TPair<FString, FUeAgentCanonicalValueRef>> Items;
				FUeAgentCanonicalValuePtr Ignore;
				return StringArray(Value.IgnoreTargetRefs, Ignore)
					&& AddDecimal(Items, TEXT("max_distance_cm"), Value.MaxDistanceCm)
					&& AddString(Items, TEXT("trace_channel"), Value.TraceChannel)
					&& AddBoolean(Items, TEXT("trace_complex"), Value.bTraceComplex)
					&& Add(Items, TEXT("ignore_target_refs"), Ignore)
					&& Object(Items, Out);
			}

			bool Add(
				TArray<TPair<FString, FUeAgentCanonicalValueRef>>& Items,
				const FString& Name,
				const FUeAgentCanonicalValuePtr& Value)
			{
				if (!Value.IsValid())
				{
					return FailBuild(TEXT("typed_intent_value_missing"));
				}
				Items.Emplace(Name, Value.ToSharedRef());
				return true;
			}

			bool AddString(
				TArray<TPair<FString, FUeAgentCanonicalValueRef>>& Items,
				const FString& Name,
				const FString& Value)
			{
				FUeAgentCanonicalValuePtr Built;
				return String(Value, Built) && Add(Items, Name, Built);
			}

			bool AddBoolean(
				TArray<TPair<FString, FUeAgentCanonicalValueRef>>& Items,
				const FString& Name,
				const bool Value)
			{
				FUeAgentCanonicalValuePtr Built;
				return Boolean(Value, Built) && Add(Items, Name, Built);
			}

			bool AddDecimal(
				TArray<TPair<FString, FUeAgentCanonicalValueRef>>& Items,
				const FString& Name,
				const double Value)
			{
				FUeAgentCanonicalValuePtr Built;
				return Decimal(Value, Built) && Add(Items, Name, Built);
			}

			bool AddInteger(
				TArray<TPair<FString, FUeAgentCanonicalValueRef>>& Items,
				const FString& Name,
				const int64 Value)
			{
				FUeAgentCanonicalValuePtr Built;
				return Integer(Value, Built) && Add(Items, Name, Built);
			}

		private:
			bool FailBuild(const FString& Detail)
			{
				Error.Code = TEXT("world_spatial_typed_intent_build_failed");
				Error.SubjectId = RequestKind;
				Error.Detail = Detail;
				return false;
			}

			FUeAgentResourceTracker& Resources;
			FUeAgentWorldSpatialExecutionError& Error;
			FString RequestKind;
		};

		bool TryBuildRevisionEntries(
			FTypedIntentBuilder& Builder,
			const TArray<FWorldSpatialExpectedRevisionEntry>& Values,
			FUeAgentCanonicalValuePtr& Out)
		{
			TArray<FUeAgentCanonicalValueRef> Items;
			for (const FWorldSpatialExpectedRevisionEntry& Value : Values)
			{
				TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields;
				FUeAgentCanonicalValuePtr Revision;
				FUeAgentCanonicalValuePtr Item;
				if (!Builder.Revision(Value.Revision, Revision)
					|| !Builder.AddString(Fields, TEXT("target_ref"), Value.TargetRef)
					|| !Builder.Add(Fields, TEXT("revision"), Revision)
					|| !Builder.Object(Fields, Item))
				{
					return false;
				}
				Items.Add(Item.ToSharedRef());
			}
			return Builder.Array(Items, Out);
		}

		bool TryBuildViewportState(
			FTypedIntentBuilder& Builder,
			const FWorldSpatialViewportState& Value,
			FUeAgentCanonicalValuePtr& Out)
		{
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> CameraFields;
			FUeAgentCanonicalValuePtr CameraTransform;
			FUeAgentCanonicalValuePtr CameraObservedAt;
			FUeAgentCanonicalValuePtr Camera;
			if (!Builder.SpatialValue(Value.Camera.CameraTransform, CameraTransform)
				|| !Builder.Temporal(Value.Camera.ObservedAt, CameraObservedAt)
				|| !Builder.Add(CameraFields, TEXT("camera_transform"), CameraTransform)
				|| !Builder.AddDecimal(CameraFields, TEXT("fov_degrees"), Value.Camera.FovDegrees)
				|| !Builder.AddString(CameraFields, TEXT("view_mode"), Value.Camera.ViewMode)
				|| !Builder.Add(CameraFields, TEXT("observed_at"), CameraObservedAt)
				|| !Builder.Object(CameraFields, Camera))
			{
				return false;
			}
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> InteractionFields;
			FUeAgentCanonicalValuePtr Interaction;
			if (!Builder.AddBoolean(InteractionFields, TEXT("mode_widgets"), Value.Interaction.bModeWidgets)
				|| !Builder.AddBoolean(InteractionFields, TEXT("selection"), Value.Interaction.bSelection)
				|| !Builder.AddBoolean(InteractionFields, TEXT("pivot"), Value.Interaction.bPivot)
				|| !Builder.AddBoolean(InteractionFields, TEXT("editor_primitives"), Value.Interaction.bEditorPrimitives)
				|| !Builder.AddBoolean(InteractionFields, TEXT("game_primitives"), Value.Interaction.bGamePrimitives)
				|| !Builder.AddString(InteractionFields, TEXT("widget_mode"), Value.Interaction.WidgetMode)
				|| !Builder.Object(InteractionFields, Interaction))
			{
				return false;
			}
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields;
			FUeAgentCanonicalValuePtr ShowFlags;
			FUeAgentCanonicalValuePtr Revision;
			return Builder.StringArray(Value.ShowFlags, ShowFlags)
				&& Builder.Revision(Value.ObservedRevision, Revision)
				&& Builder.AddString(Fields, TEXT("viewport_ref"), Value.ViewportRef)
				&& Builder.Add(Fields, TEXT("camera"), Camera)
				&& Builder.AddBoolean(Fields, TEXT("realtime"), Value.bRealtime)
				&& Builder.AddBoolean(Fields, TEXT("game_view"), Value.bGameView)
				&& Builder.Add(Fields, TEXT("show_flags"), ShowFlags)
				&& Builder.Add(Fields, TEXT("interaction"), Interaction)
				&& Builder.Add(Fields, TEXT("observed_revision"), Revision)
				&& Builder.Object(Fields, Out);
		}

		bool TryBuildTypedPayload(
			const FUeAgentWorldSpatialRequestPayload& Payload,
			FTypedIntentBuilder& Builder,
			FUeAgentCanonicalValuePtr& Out)
		{
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields;
			if (Payload.IsType<FGraphViewUpdateRequestInput>())
			{
				const FGraphViewUpdateRequestInput& Input =
					Payload.Get<FGraphViewUpdateRequestInput>();
				const FWorldSpatialGraphViewState& Value = Input.ViewState;
				TArray<TPair<FString, FUeAgentCanonicalValueRef>> StateFields;
				FUeAgentCanonicalValuePtr Location;
				FUeAgentCanonicalValuePtr Revision;
				FUeAgentCanonicalValuePtr State;
				return Builder.SpatialValue(Value.Location, Location)
					&& Builder.Revision(Value.GraphRevision, Revision)
					&& Builder.Add(StateFields, TEXT("location"), Location)
					&& Builder.AddDecimal(StateFields, TEXT("zoom"), Value.Zoom)
					&& Builder.Add(StateFields, TEXT("graph_revision"), Revision)
					&& Builder.Object(StateFields, State)
					&& Builder.Add(Fields, TEXT("view_state"), State)
					&& Builder.AddBoolean(
						Fields, TEXT("update_document_state"),
						Input.bUpdateDocumentState)
					&& Builder.AddBoolean(
						Fields, TEXT("save_after_update"),
						Input.bSaveAfterUpdate)
					&& Builder.Object(Fields, Out);
			}
			if (Payload.IsType<FGraphLayoutApplyRequestInput>())
			{
				const FGraphLayoutApplyRequestInput& Value =
					Payload.Get<FGraphLayoutApplyRequestInput>();
				TArray<FUeAgentCanonicalValueRef> Placements;
				for (const FWorldSpatialNodePlacement& Placement : Value.NodePlacements)
				{
					TArray<TPair<FString, FUeAgentCanonicalValueRef>> ItemFields;
					FUeAgentCanonicalValuePtr Position;
					FUeAgentCanonicalValuePtr Item;
					if (!Builder.SpatialValue(Placement.Position, Position)
						|| !Builder.AddString(ItemFields, TEXT("node_ref"), Placement.NodeRef)
						|| !Builder.Add(ItemFields, TEXT("position"), Position)
						|| !Builder.Object(ItemFields, Item))
					{
						return false;
					}
					Placements.Add(Item.ToSharedRef());
				}
				TArray<FUeAgentCanonicalValueRef> Relations;
				for (const FWorldSpatialLayoutRelation& Relation : Value.LayoutRelations)
				{
					TArray<TPair<FString, FUeAgentCanonicalValueRef>> ItemFields;
					FUeAgentCanonicalValuePtr Item;
					if (!Builder.AddString(ItemFields, TEXT("source_node_ref"), Relation.SourceNodeRef)
						|| !Builder.AddString(ItemFields, TEXT("target_node_ref"), Relation.TargetNodeRef)
						|| !Builder.AddString(ItemFields, TEXT("relation"), Relation.Relation)
						|| (Relation.Gap.IsSet()
							&& !Builder.AddDecimal(ItemFields, TEXT("gap"), Relation.Gap.GetValue()))
						|| !Builder.Object(ItemFields, Item))
					{
						return false;
					}
					Relations.Add(Item.ToSharedRef());
				}
				FUeAgentCanonicalValuePtr PlacementArray;
				FUeAgentCanonicalValuePtr RelationArray;
				FUeAgentCanonicalValuePtr Revision;
				return Builder.Array(Placements, PlacementArray)
					&& Builder.Array(Relations, RelationArray)
					&& Builder.Revision(Value.ExpectedRevision, Revision)
					&& Builder.AddString(Fields, TEXT("graph_ref"), Value.GraphRef)
					&& Builder.Add(Fields, TEXT("node_placements"), PlacementArray)
					&& Builder.Add(Fields, TEXT("expected_revision"), Revision)
					&& Builder.Add(Fields, TEXT("layout_relations"), RelationArray)
					&& Builder.Object(Fields, Out);
			}
			if (Payload.IsType<FWorldObjectTransformUpdateRequestInput>())
			{
				const FWorldObjectTransformUpdateRequestInput& Value =
					Payload.Get<FWorldObjectTransformUpdateRequestInput>();
				FUeAgentCanonicalValuePtr Transform;
				FUeAgentCanonicalValuePtr Revision;
				return Builder.SpatialValue(Value.Transform, Transform)
					&& Builder.Revision(Value.ExpectedRevision, Revision)
					&& Builder.Add(Fields, TEXT("transform"), Transform)
					&& Builder.Add(Fields, TEXT("expected_revision"), Revision)
					&& Builder.Object(Fields, Out);
			}
			if (Payload.IsType<FWorldObjectAlignRequestInput>())
			{
				const FWorldObjectAlignRequestInput& Value =
					Payload.Get<FWorldObjectAlignRequestInput>();
				const FWorldSpatialAlignment& Alignment = Value.Alignment;
				TArray<TPair<FString, FUeAgentCanonicalValueRef>> AlignmentFields;
				FUeAgentCanonicalValuePtr VerifyFrame;
				FUeAgentCanonicalValuePtr AlignmentValue;
				FUeAgentCanonicalValuePtr Subjects;
				FUeAgentCanonicalValuePtr Revisions;
				if (!Builder.Frame(Alignment.VerifyFrame, VerifyFrame)
					|| !Builder.AddString(AlignmentFields, TEXT("kind"), Alignment.Kind)
					|| !Builder.Add(AlignmentFields, TEXT("verify_frame"), VerifyFrame))
				{
					return false;
				}
				if (Alignment.SubjectAnchor.IsSet())
				{
					FUeAgentCanonicalValuePtr Anchor;
					if (!Builder.SpatialValue(Alignment.SubjectAnchor.GetValue(), Anchor)
						|| !Builder.Add(AlignmentFields, TEXT("subject_anchor"), Anchor))
					{
						return false;
					}
				}
				if (Alignment.ReferenceAnchor.IsSet())
				{
					FUeAgentCanonicalValuePtr Anchor;
					if (!Builder.SpatialValue(Alignment.ReferenceAnchor.GetValue(), Anchor)
						|| !Builder.Add(AlignmentFields, TEXT("reference_anchor"), Anchor))
					{
						return false;
					}
				}
				if (Alignment.SubjectNormal.IsSet())
				{
					FUeAgentCanonicalValuePtr Normal;
					if (!Builder.SpatialValue(Alignment.SubjectNormal.GetValue(), Normal)
						|| !Builder.Add(AlignmentFields, TEXT("subject_normal"), Normal))
					{
						return false;
					}
				}
				if (Alignment.ReferenceNormal.IsSet())
				{
					FUeAgentCanonicalValuePtr Normal;
					if (!Builder.SpatialValue(Alignment.ReferenceNormal.GetValue(), Normal)
						|| !Builder.Add(AlignmentFields, TEXT("reference_normal"), Normal))
					{
						return false;
					}
				}
				return (Alignment.Axis.IsEmpty()
						|| Builder.AddString(AlignmentFields, TEXT("axis"), Alignment.Axis))
					&& (Alignment.SubjectBoundsAnchor.IsEmpty()
						|| Builder.AddString(AlignmentFields, TEXT("source_anchor"), Alignment.SubjectBoundsAnchor))
					&& (Alignment.ReferenceBoundsAnchor.IsEmpty()
						|| Builder.AddString(AlignmentFields, TEXT("target_anchor"), Alignment.ReferenceBoundsAnchor))
					&& (Alignment.SubjectFace.IsEmpty()
						|| Builder.AddString(AlignmentFields, TEXT("source_face"), Alignment.SubjectFace))
					&& (Alignment.ReferenceFace.IsEmpty()
						|| Builder.AddString(AlignmentFields, TEXT("target_face"), Alignment.ReferenceFace))
					&& Builder.AddDecimal(AlignmentFields, TEXT("offset_cm"), Alignment.OffsetCm)
					&& (!Alignment.SubjectVertexIndex.IsSet()
						|| Builder.AddInteger(AlignmentFields, TEXT("source_vertex_index"), Alignment.SubjectVertexIndex.GetValue()))
					&& (!Alignment.ReferenceVertexIndex.IsSet()
						|| Builder.AddInteger(AlignmentFields, TEXT("target_vertex_index"), Alignment.ReferenceVertexIndex.GetValue()))
					&& Builder.AddString(AlignmentFields, TEXT("rotation_policy"), Alignment.RotationPolicy)
					&& Builder.Object(AlignmentFields, AlignmentValue)
					&& Builder.StringArray(Value.SubjectRefs, Subjects)
					&& TryBuildRevisionEntries(Builder, Value.ExpectedRevisions, Revisions)
					&& Builder.Add(Fields, TEXT("subject_refs"), Subjects)
					&& Builder.Add(Fields, TEXT("alignment"), AlignmentValue)
					&& Builder.Add(Fields, TEXT("expected_revisions"), Revisions)
					&& (!Value.AnchorRef.IsSet()
						|| Builder.AddString(Fields, TEXT("anchor_ref"), Value.AnchorRef.GetValue()))
					&& Builder.Object(Fields, Out);
			}
			if (Payload.IsType<FWorldObjectSnapToSurfaceRequestInput>())
			{
				const FWorldObjectSnapToSurfaceRequestInput& Value =
					Payload.Get<FWorldObjectSnapToSurfaceRequestInput>();
				TArray<TPair<FString, FUeAgentCanonicalValueRef>> QueryFields;
				FUeAgentCanonicalValuePtr Origin;
				FUeAgentCanonicalValuePtr Direction;
				FUeAgentCanonicalValuePtr Ignore;
				FUeAgentCanonicalValuePtr Query;
				TArray<TPair<FString, FUeAgentCanonicalValueRef>> PolicyFields;
				FUeAgentCanonicalValuePtr Policy;
				FUeAgentCanonicalValuePtr Subjects;
				FUeAgentCanonicalValuePtr Revisions;
				return Builder.SpatialValue(Value.SurfaceQuery.Origin, Origin)
					&& Builder.SpatialValue(Value.SurfaceQuery.Direction, Direction)
					&& Builder.StringArray(Value.SurfaceQuery.IgnoreTargetRefs, Ignore)
					&& Builder.Add(QueryFields, TEXT("origin"), Origin)
					&& Builder.Add(QueryFields, TEXT("direction"), Direction)
					&& Builder.AddDecimal(QueryFields, TEXT("max_distance_cm"), Value.SurfaceQuery.MaxDistanceCm)
					&& Builder.AddString(QueryFields, TEXT("trace_channel"), Value.SurfaceQuery.TraceChannel)
					&& Builder.AddBoolean(QueryFields, TEXT("trace_complex"), Value.SurfaceQuery.bTraceComplex)
					&& Builder.Add(QueryFields, TEXT("ignore_target_refs"), Ignore)
					&& Builder.Object(QueryFields, Query)
					&& Builder.AddDecimal(PolicyFields, TEXT("clearance_cm"), Value.PlacementPolicy.ClearanceCm)
					&& Builder.AddBoolean(PolicyFields, TEXT("align_to_surface_normal"), Value.PlacementPolicy.bAlignToSurfaceNormal)
					&& Builder.AddBoolean(PolicyFields, TEXT("preserve_yaw"), Value.PlacementPolicy.bPreserveYaw)
					&& Builder.Object(PolicyFields, Policy)
					&& Builder.StringArray(Value.SubjectRefs, Subjects)
					&& TryBuildRevisionEntries(Builder, Value.ExpectedRevisions, Revisions)
					&& Builder.Add(Fields, TEXT("subject_refs"), Subjects)
					&& Builder.Add(Fields, TEXT("surface_query"), Query)
					&& Builder.Add(Fields, TEXT("placement_policy"), Policy)
					&& Builder.Add(Fields, TEXT("expected_revisions"), Revisions)
					&& Builder.Object(Fields, Out);
			}
			if (Payload.IsType<FMeshTransformBakeRequestInput>())
			{
				const FMeshTransformBakeRequestInput& Value =
					Payload.Get<FMeshTransformBakeRequestInput>();
				FUeAgentCanonicalValuePtr Transform;
				FUeAgentCanonicalValuePtr Revision;
				return Builder.SpatialValue(Value.Transform, Transform)
					&& Builder.Revision(Value.ExpectedRevision, Revision)
					&& Builder.Add(Fields, TEXT("transform"), Transform)
					&& Builder.Add(Fields, TEXT("expected_revision"), Revision)
					&& (!Value.PreservePlacement.IsSet()
						|| Builder.AddBoolean(Fields, TEXT("preserve_placement"), Value.PreservePlacement.GetValue()))
					&& Builder.Object(Fields, Out);
			}
			if (Payload.IsType<FMeshPivotUpdateRequestInput>())
			{
				const FMeshPivotUpdateRequestInput& Value =
					Payload.Get<FMeshPivotUpdateRequestInput>();
				FUeAgentCanonicalValuePtr Pivot;
				FUeAgentCanonicalValuePtr Revision;
				return Builder.SpatialValue(Value.Pivot, Pivot)
					&& Builder.Revision(Value.ExpectedRevision, Revision)
					&& Builder.Add(Fields, TEXT("pivot"), Pivot)
					&& Builder.Add(Fields, TEXT("expected_revision"), Revision)
					&& (!Value.PreserveWorldGeometry.IsSet()
						|| Builder.AddBoolean(Fields, TEXT("preserve_world_geometry"), Value.PreserveWorldGeometry.GetValue()))
					&& Builder.Object(Fields, Out);
			}
			if (Payload.IsType<FViewportActorSelectRequestInput>())
			{
				const FViewportActorSelectRequestInput& Value =
					Payload.Get<FViewportActorSelectRequestInput>();
				if (Value.ScreenPoint.IsSet())
				{
					FUeAgentCanonicalValuePtr ScreenPoint;
					if (!Builder.SpatialValue(Value.ScreenPoint.GetValue(), ScreenPoint)
						|| !Builder.Add(Fields, TEXT("screen_point"), ScreenPoint))
					{
						return false;
					}
				}
				if (!Value.ActorRefs.IsEmpty())
				{
					FUeAgentCanonicalValuePtr Actors;
					if (!Builder.StringArray(Value.ActorRefs, Actors)
						|| !Builder.Add(Fields, TEXT("actor_refs"), Actors))
					{
						return false;
					}
				}
				if (!Builder.AddString(Fields, TEXT("selection_mode"), Value.SelectionMode))
				{
					return false;
				}
				if (Value.TraceQuery.IsSet())
				{
					FUeAgentCanonicalValuePtr Query;
					if (!Builder.TraceQuery(Value.TraceQuery.GetValue(), Query)
						|| !Builder.Add(Fields, TEXT("trace_query"), Query))
					{
						return false;
					}
				}
				return Builder.Object(Fields, Out);
			}
			if (Payload.IsType<FMeshElementSelectByRayRequestInput>())
			{
				const FMeshElementSelectByRayRequestInput& Value =
					Payload.Get<FMeshElementSelectByRayRequestInput>();
				FUeAgentCanonicalValuePtr ScreenPoint;
				if (!Builder.SpatialValue(Value.ScreenPoint, ScreenPoint)
					|| !Builder.Add(Fields, TEXT("screen_point"), ScreenPoint)
					|| !Builder.AddString(Fields, TEXT("element_kind"), Value.ElementKind)
					|| !Builder.AddString(Fields, TEXT("selection_mode"), Value.SelectionMode))
				{
					return false;
				}
				if (Value.TraceQuery.IsSet())
				{
					FUeAgentCanonicalValuePtr Query;
					if (!Builder.TraceQuery(Value.TraceQuery.GetValue(), Query)
						|| !Builder.Add(Fields, TEXT("trace_query"), Query))
					{
						return false;
					}
				}
				return Builder.Object(Fields, Out);
			}
			if (Payload.IsType<FViewportCameraFrameRequestInput>())
			{
				const FViewportCameraFrameRequestInput& Value =
					Payload.Get<FViewportCameraFrameRequestInput>();
				TArray<TPair<FString, FUeAgentCanonicalValueRef>> PolicyFields;
				FUeAgentCanonicalValuePtr Policy;
				if (!Builder.AddDecimal(PolicyFields, TEXT("padding_ratio"), Value.FramePolicy.PaddingRatio)
					|| !Builder.AddDecimal(PolicyFields, TEXT("minimum_distance_cm"), Value.FramePolicy.MinimumDistanceCm)
					|| !Builder.AddDecimal(PolicyFields, TEXT("maximum_distance_cm"), Value.FramePolicy.MaximumDistanceCm)
					|| !Builder.AddBoolean(PolicyFields, TEXT("preserve_rotation"), Value.FramePolicy.bPreserveRotation)
					|| !Builder.AddBoolean(PolicyFields, TEXT("collision_aware"), Value.FramePolicy.bCollisionAware)
					|| !Builder.AddBoolean(PolicyFields, TEXT("look_at"), Value.FramePolicy.bLookAt)
					|| !Builder.AddBoolean(PolicyFields, TEXT("auto_fallback"), Value.FramePolicy.bAutoFallback)
					|| !Builder.Object(PolicyFields, Policy))
				{
					return false;
				}
				if (!Value.SubjectRefs.IsEmpty())
				{
					FUeAgentCanonicalValuePtr Subjects;
					if (!Builder.StringArray(Value.SubjectRefs, Subjects)
						|| !Builder.Add(Fields, TEXT("subject_refs"), Subjects))
					{
						return false;
					}
				}
				if (Value.FolderPath.IsSet()
					&& !Builder.AddString(Fields, TEXT("folder_path"), Value.FolderPath.GetValue()))
				{
					return false;
				}
				if (Value.FolderPath.IsSet()
					&& !Builder.AddBoolean(Fields, TEXT("include_child_folders"), Value.bIncludeChildFolders))
				{
					return false;
				}
				if (Value.bUseCurrentSelection
					&& !Builder.AddBoolean(Fields, TEXT("use_current_selection"), true))
				{
					return false;
				}
				return Builder.Add(Fields, TEXT("frame_policy"), Policy)
					&& Builder.Object(Fields, Out);
			}
			if (Payload.IsType<FViewportStateRestoreRequestInput>())
			{
				FUeAgentCanonicalValuePtr Snapshot;
				return TryBuildViewportState(
						Builder,
						Payload.Get<FViewportStateRestoreRequestInput>().Snapshot,
						Snapshot)
					&& Builder.Add(Fields, TEXT("snapshot"), Snapshot)
					&& Builder.Object(Fields, Out);
			}
			if (Payload.IsType<FViewportStateUpdateRequestInput>())
			{
				const FWorldSpatialViewportStatePatch& Value =
					Payload.Get<FViewportStateUpdateRequestInput>().StatePatch;
				TArray<TPair<FString, FUeAgentCanonicalValueRef>> PatchFields;
				if (Value.CameraTransform.IsSet())
				{
					FUeAgentCanonicalValuePtr Camera;
					if (!Builder.SpatialValue(Value.CameraTransform.GetValue(), Camera)
						|| !Builder.Add(PatchFields, TEXT("camera_transform"), Camera))
					{
						return false;
					}
				}
				if (Value.FovDegrees.IsSet()
					&& !Builder.AddDecimal(PatchFields, TEXT("fov_degrees"), Value.FovDegrees.GetValue()))
				{
					return false;
				}
				if (Value.ViewMode.IsSet()
					&& !Builder.AddString(PatchFields, TEXT("view_mode"), Value.ViewMode.GetValue()))
				{
					return false;
				}
				if (Value.Realtime.IsSet()
					&& !Builder.AddBoolean(PatchFields, TEXT("realtime"), Value.Realtime.GetValue()))
				{
					return false;
				}
				if (Value.GameView.IsSet()
					&& !Builder.AddBoolean(PatchFields, TEXT("game_view"), Value.GameView.GetValue()))
				{
					return false;
				}
				if (Value.ShowFlags.IsSet())
				{
					FUeAgentCanonicalValuePtr ShowFlags;
					if (!Builder.StringArray(Value.ShowFlags.GetValue(), ShowFlags)
						|| !Builder.Add(PatchFields, TEXT("show_flags"), ShowFlags))
					{
						return false;
					}
				}
				if (Value.Interaction.IsSet())
				{
					const FWorldSpatialViewportInteractionState& InteractionValue =
						Value.Interaction.GetValue();
					TArray<TPair<FString, FUeAgentCanonicalValueRef>> InteractionFields;
					FUeAgentCanonicalValuePtr Interaction;
					if (!Builder.AddBoolean(InteractionFields, TEXT("mode_widgets"), InteractionValue.bModeWidgets)
						|| !Builder.AddBoolean(InteractionFields, TEXT("selection"), InteractionValue.bSelection)
						|| !Builder.AddBoolean(InteractionFields, TEXT("pivot"), InteractionValue.bPivot)
						|| !Builder.AddBoolean(InteractionFields, TEXT("editor_primitives"), InteractionValue.bEditorPrimitives)
						|| !Builder.AddBoolean(InteractionFields, TEXT("game_primitives"), InteractionValue.bGamePrimitives)
						|| !Builder.AddString(InteractionFields, TEXT("widget_mode"), InteractionValue.WidgetMode)
						|| !Builder.Object(InteractionFields, Interaction)
						|| !Builder.Add(PatchFields, TEXT("interaction"), Interaction))
					{
						return false;
					}
				}
				FUeAgentCanonicalValuePtr Patch;
				return Builder.Object(PatchFields, Patch)
					&& Builder.Add(Fields, TEXT("state_patch"), Patch)
					&& Builder.Object(Fields, Out);
			}
			return false;
		}
	}

	FUeAgentWorldSpatialRequest::FUeAgentWorldSpatialRequest(
		FString InTypeId,
		FString InFamilyId,
		FString InRequestKindId,
		const bool bInOperation,
		FUeAgentWorldSpatialRequestPayload InPayload)
		: TypeId(MoveTemp(InTypeId))
		, FamilyId(MoveTemp(InFamilyId))
		, RequestKindId(MoveTemp(InRequestKindId))
		, bOperation(bInOperation)
		, Payload(MoveTemp(InPayload))
	{
	}

	const FString& FUeAgentWorldSpatialRequest::GetTypeId() const { return TypeId; }
	const FString& FUeAgentWorldSpatialRequest::GetFamilyId() const { return FamilyId; }
	const FString& FUeAgentWorldSpatialRequest::GetRequestKindId() const { return RequestKindId; }
	const FUeAgentWorldSpatialRequestPayload& FUeAgentWorldSpatialRequest::GetPayload() const { return Payload; }
	bool FUeAgentWorldSpatialRequest::IsOperation() const { return bOperation; }

	bool FUeAgentWorldSpatialRequest::TryBuildChangeDraft(
		const UE::UAI::StateSemantic::FUeAgentCanonicalSnapshot& Snapshot,
		const UE::UAI::ContextResource::FUeAgentOperationContext& Context,
		TOptional<UE::UAI::PlanningPreflight::FChangeDraft>& OutDraft,
		FUeAgentWorldSpatialExecutionError& OutError) const
	{
		OutDraft.Reset();
		OutError = {};
		if (!bOperation)
		{
			return Fail(
				OutError,
				TEXT("world_spatial_query_has_no_change_draft"),
				RequestKindId,
				TEXT("query requests do not create C6 changes"));
		}
		if (Snapshot.GetEntries().IsEmpty())
		{
			return Fail(
				OutError,
				TEXT("world_spatial_preflight_snapshot_empty"),
				RequestKindId);
		}

		FTypedIntentBuilder Builder(
			Context.GetResourceTracker(),
			OutError,
			RequestKindId);
		UE::UAI::StateSemantic::FUeAgentCanonicalValuePtr TypedPayload;
		if (!TryBuildTypedPayload(Payload, Builder, TypedPayload)
			|| !TypedPayload.IsValid())
		{
			if (!OutError.IsValid())
			{
				Fail(
					OutError,
					TEXT("world_spatial_typed_intent_unsupported"),
					RequestKindId);
			}
			return false;
		}

		TArray<TPair<
			FString,
			UE::UAI::StateSemantic::FUeAgentCanonicalValueRef>> IntentFields;
		if (!Builder.AddString(IntentFields, TEXT("request_kind"), RequestKindId)
			|| !Builder.AddString(IntentFields, TEXT("family_id"), FamilyId)
			|| !Builder.Add(IntentFields, TEXT("typed_payload"), TypedPayload))
		{
			return false;
		}
		UE::UAI::StateSemantic::FUeAgentCanonicalValuePtr Proposed;
		if (!Builder.Object(IntentFields, Proposed) || !Proposed.IsValid())
		{
			return false;
		}

		const UE::UAI::StateSemantic::FUeAgentStateEntry& Primary =
			Snapshot.GetEntries()[0];
		TArray<UE::UAI::StateSemantic::FUeAgentCanonicalIdentity> ImpactTargets;
		for (const UE::UAI::StateSemantic::FUeAgentStateEntry& Entry
			: Snapshot.GetEntries())
		{
			ImpactTargets.Add(Entry.GetIdentity());
		}
		OutDraft.Emplace(
			Primary.GetIdentity(),
			UE::UAI::PlanningPreflight::EChangeIntent::Update,
			Primary.GetValue(),
			Proposed,
			TArray<FString>{TEXT("world-spatial:write")},
			TEXT("world-spatial-typed-intent:") + RequestKindId,
			MoveTemp(ImpactTargets));
		return OutDraft->IsValid();
	}

	bool TryCreateWorldSpatialRequestDecoder(
		TSharedPtr<IUeAgentWorldSpatialRequestDecoder>& OutDecoder,
		FUeAgentWorldSpatialExecutionError& OutError)
	{
		OutDecoder.Reset();
		OutError = {};
		if (RequestSpecs().Num() != 22)
		{
			return Fail(
				OutError,
				TEXT("world_spatial_request_spec_count_invalid"),
				TEXT("world_spatial"));
		}
		TSet<FString> Capabilities;
		TSet<FString> Types;
		TSet<FString> Kinds;
		TSet<FString> Families;
		for (const FRequestSpec& Spec : RequestSpecs())
		{
			if (Capabilities.Contains(Spec.CapabilityId)
				|| Types.Contains(Spec.TypeId)
				|| Kinds.Contains(Spec.RequestKindId)
				|| !Spec.Decode)
			{
				return Fail(
					OutError,
					TEXT("world_spatial_request_spec_duplicate"),
					Spec.CapabilityId);
			}
			Capabilities.Add(Spec.CapabilityId);
			Types.Add(Spec.TypeId);
			Kinds.Add(Spec.RequestKindId);
			Families.Add(Spec.FamilyId);
		}
		if (Families.Num() != 5)
		{
			return Fail(
				OutError,
				TEXT("world_spatial_request_family_count_invalid"),
				TEXT("world_spatial"));
		}
		OutDecoder = MakeShared<FWorldSpatialRequestDecoder>();
		return true;
	}
}

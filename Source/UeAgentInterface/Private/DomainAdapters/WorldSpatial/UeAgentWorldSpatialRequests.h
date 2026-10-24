#pragma once

#include "CoreMinimal.h"
#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialExecutionChains.h"
#include "Misc/TVariant.h"

#define UAI_WORLD_SPATIAL_TYPED_REQUESTS_V1 1

namespace UE::UAI::DomainAdapters::WorldSpatial
{
	struct UEAGENTINTERFACE_API FWorldSpatialRevision
	{
		FString SourceId;
		FString Generation;
		int64 Sequence = -1;

		bool TryValidate(FUeAgentWorldSpatialExecutionError& OutError) const;
		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FWorldSpatialTemporalPoint
	{
		FString ClockId;
		FString Tick;
		FString RateNumerator;
		FString RateDenominator;
		FString State;

		bool TryValidate(FUeAgentWorldSpatialExecutionError& OutError) const;
		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FWorldSpatialFrame
	{
		FString FrameId;
		FString BasisFrameId;
		int32 Dimension = 0;
		FString Unit;
		FString BasisEvidenceId;
		FWorldSpatialTemporalPoint ObservedAt;
		// An authored coordinate frame carries no observation claim. Native
		// conversion supplies its own evidence after resolving the real context.
		bool bAuthored = false;

		bool TryValidate(FUeAgentWorldSpatialExecutionError& OutError) const;
		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FWorldSpatialValue
	{
		FString Subject;
		FString Quantity;
		FWorldSpatialFrame Frame;
		TArray<FString> Components;
		TArray<FString> ConversionEvidenceIds;

		bool TryValidate(FUeAgentWorldSpatialExecutionError& OutError) const;
		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FWorldSpatialExpectedRevisionEntry
	{
		FString TargetRef;
		FWorldSpatialRevision Revision;

		bool TryValidate(FUeAgentWorldSpatialExecutionError& OutError) const;
		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FWorldSpatialGraphViewState
	{
		FWorldSpatialValue Location;
		double Zoom = 0.0;

		bool TryValidate(FUeAgentWorldSpatialExecutionError& OutError) const;
		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FWorldSpatialNodePlacement
	{
		FString NodeRef;
		FWorldSpatialValue Position;

		bool TryValidate(FUeAgentWorldSpatialExecutionError& OutError) const;
		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FWorldSpatialLayoutRelation
	{
		FString SourceNodeRef;
		FString TargetNodeRef;
		FString Relation;
		TOptional<double> Gap;

		bool TryValidate(FUeAgentWorldSpatialExecutionError& OutError) const;
		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FWorldSpatialAlignment
	{
		FString Kind;
		FWorldSpatialFrame VerifyFrame;
		TOptional<FWorldSpatialValue> SubjectAnchor;
		TOptional<FWorldSpatialValue> ReferenceAnchor;
		TOptional<FWorldSpatialValue> SubjectNormal;
		TOptional<FWorldSpatialValue> ReferenceNormal;
		FString RotationPolicy = TEXT("preserve");
		FString Axis;
		FString SubjectBoundsAnchor;
		FString ReferenceBoundsAnchor;
		FString SubjectFace;
		FString ReferenceFace;
		double OffsetCm = 0.0;
		TOptional<int32> SubjectVertexIndex;
		TOptional<int32> ReferenceVertexIndex;

		bool TryValidate(FUeAgentWorldSpatialExecutionError& OutError) const;
		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FWorldSpatialSurfaceQuery
	{
		FWorldSpatialValue Origin;
		FWorldSpatialValue Direction;
		double MaxDistanceCm = 0.0;
		FString TraceChannel;
		bool bTraceComplex = false;
		TArray<FString> IgnoreTargetRefs;

		bool TryValidate(FUeAgentWorldSpatialExecutionError& OutError) const;
		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FWorldSpatialPlacementPolicy
	{
		double ClearanceCm = 0.0;
		bool bAlignToSurfaceNormal = false;
		bool bPreserveYaw = false;

		bool TryValidate(FUeAgentWorldSpatialExecutionError& OutError) const;
		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FWorldSpatialTraceQuery
	{
		double MaxDistanceCm = 0.0;
		FString TraceChannel;
		bool bTraceComplex = false;
		TArray<FString> IgnoreTargetRefs;

		bool TryValidate(FUeAgentWorldSpatialExecutionError& OutError) const;
		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FWorldSpatialFramePolicy
	{
		double PaddingRatio = 0.0;
		double MinimumDistanceCm = 0.0;
		double MaximumDistanceCm = 0.0;
		bool bPreserveRotation = false;
		bool bCollisionAware = false;
		bool bLookAt = false;
		bool bAutoFallback = false;

		bool TryValidate(FUeAgentWorldSpatialExecutionError& OutError) const;
		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FWorldSpatialViewportInteractionState
	{
		bool bModeWidgets = true;
		bool bSelection = true;
		bool bPivot = true;
		bool bEditorPrimitives = true;
		bool bGamePrimitives = false;
		FString WidgetMode;

		bool TryValidate(FUeAgentWorldSpatialExecutionError& OutError) const;
		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FWorldSpatialViewportCameraState
	{
		FWorldSpatialValue CameraTransform;
		double FovDegrees = 0.0;
		FString ViewMode;
		FWorldSpatialTemporalPoint ObservedAt;

		bool TryValidate(FUeAgentWorldSpatialExecutionError& OutError) const;
		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FWorldSpatialViewportState
	{
		FString ViewportRef;
		FWorldSpatialViewportCameraState Camera;
		bool bRealtime = false;
		bool bGameView = false;
		TArray<FString> ShowFlags;
		FWorldSpatialViewportInteractionState Interaction;
		FWorldSpatialRevision ObservedRevision;

		bool TryValidate(FUeAgentWorldSpatialExecutionError& OutError) const;
		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FWorldSpatialViewportStatePatch
	{
		TOptional<FWorldSpatialValue> CameraTransform;
		TOptional<FString> CameraTransformMode;
		TOptional<double> FovDegrees;
		TOptional<FString> ViewMode;
		TOptional<bool> Realtime;
		TOptional<bool> GameView;
		TOptional<TArray<FString>> ShowFlags;
		TOptional<FWorldSpatialViewportInteractionState> Interaction;

		bool TryValidate(FUeAgentWorldSpatialExecutionError& OutError) const;
		bool IsValid() const;
	};

	struct FGraphViewReadRequestInput
	{
		bool bIncludeDocumentState = true;
	};
	struct FGraphViewUpdateRequestInput
	{
		FWorldSpatialGraphViewState ViewState;
		bool bUpdateDocumentState = false;
		bool bSaveAfterUpdate = false;
	};
	struct FGraphLayoutApplyRequestInput
	{
		FString GraphRef;
		TArray<FWorldSpatialNodePlacement> NodePlacements;
		TArray<FWorldSpatialLayoutRelation> LayoutRelations;
	};
	struct FObjectTransformReadRequestInput { FWorldSpatialFrame VerifyFrame; };
	struct FWorldObjectTransformUpdateRequestInput
	{
		FWorldSpatialValue Transform;
	};
	struct FWorldObjectAlignRequestInput
	{
		TArray<FString> SubjectRefs;
		FWorldSpatialAlignment Alignment;
		TOptional<FString> AnchorRef;
	};
	struct FWorldObjectSnapToSurfaceRequestInput
	{
		TArray<FString> SubjectRefs;
		FWorldSpatialSurfaceQuery SurfaceQuery;
		FWorldSpatialPlacementPolicy PlacementPolicy;
	};
	struct FMeshTransformBakeRequestInput
	{
		FWorldSpatialValue Transform;
		TOptional<bool> PreservePlacement;
	};
	struct FMeshPivotUpdateRequestInput
	{
		FWorldSpatialValue Pivot;
		TOptional<bool> PreserveWorldGeometry;
	};
	struct FViewportRayDeprojectRequestInput { FWorldSpatialValue ScreenPoint; };
	struct FViewportRayTraceRequestInput
	{
		FWorldSpatialValue ScreenPoint;
		FWorldSpatialTraceQuery TraceQuery;
	};
	struct FViewportActorPickRequestInput
	{
		FWorldSpatialValue ScreenPoint;
		FWorldSpatialTraceQuery TraceQuery;
		bool bAllowNoHit = false;
	};
	struct UEAGENTINTERFACE_API FViewportActorSelectRequestInput
	{
		TOptional<FWorldSpatialValue> ScreenPoint;
		TArray<FString> ActorRefs;
		FString SelectionMode;
		TOptional<FWorldSpatialTraceQuery> TraceQuery;

		bool HasExactlyOneSelector() const;
	};
	struct FMeshElementSelectByRayRequestInput
	{
		FWorldSpatialValue ScreenPoint;
		FString ElementKind;
		FString SelectionMode;
		TOptional<FWorldSpatialTraceQuery> TraceQuery;
	};
	struct FViewportPixelScaleReadRequestInput { FWorldSpatialValue SamplePoint; };
	struct UEAGENTINTERFACE_API FViewportCameraFrameRequestInput
	{
		TArray<FString> SubjectRefs;
		TOptional<FString> FolderPath;
		bool bIncludeChildFolders = false;
		bool bUseCurrentSelection = false;
		TOptional<FWorldSpatialValue> WorldPoint;
		TOptional<FWorldSpatialValue> WorldBoundsMin;
		TOptional<FWorldSpatialValue> WorldBoundsMax;
		FWorldSpatialFramePolicy FramePolicy;

		bool HasExactlyOneSelector() const;
	};
	struct FViewportStateReadRequestInput {};
	struct FViewportStateRestoreRequestInput { FWorldSpatialViewportState Snapshot; };
	struct FViewportWarningListRequestInput
	{
		TOptional<FString> ViewportRef;
		bool bIncludeSuppressed = false;
	};
	struct FViewportViewModeListRequestInput { FString Query; };
	struct FViewportInstanceListRequestInput {};
	struct FViewportStateUpdateRequestInput { FWorldSpatialViewportStatePatch StatePatch; };

	using FUeAgentWorldSpatialRequestPayload = TVariant<
		FGraphViewReadRequestInput,
		FGraphViewUpdateRequestInput,
		FGraphLayoutApplyRequestInput,
		FObjectTransformReadRequestInput,
		FWorldObjectTransformUpdateRequestInput,
		FWorldObjectAlignRequestInput,
		FWorldObjectSnapToSurfaceRequestInput,
		FMeshTransformBakeRequestInput,
		FMeshPivotUpdateRequestInput,
		FViewportRayDeprojectRequestInput,
		FViewportRayTraceRequestInput,
		FViewportActorPickRequestInput,
		FViewportActorSelectRequestInput,
		FMeshElementSelectByRayRequestInput,
		FViewportPixelScaleReadRequestInput,
		FViewportCameraFrameRequestInput,
		FViewportStateReadRequestInput,
		FViewportStateRestoreRequestInput,
		FViewportWarningListRequestInput,
		FViewportViewModeListRequestInput,
		FViewportInstanceListRequestInput,
		FViewportStateUpdateRequestInput>;

	class UEAGENTINTERFACE_API FUeAgentWorldSpatialRequest final
		: public IUeAgentWorldSpatialTypedRequest
	{
	public:
		FUeAgentWorldSpatialRequest(
			FString InTypeId,
			FString InFamilyId,
			FString InRequestKindId,
			bool bInOperation,
			FUeAgentWorldSpatialRequestPayload InPayload);

		const FString& GetTypeId() const override;
		const FString& GetFamilyId() const override;
		const FString& GetRequestKindId() const override;
		const FUeAgentWorldSpatialRequestPayload& GetPayload() const;
		bool IsOperation() const;
		bool TryBuildChangeDraft(
			const UE::UAI::StateSemantic::FUeAgentCanonicalSnapshot& Snapshot,
			const UE::UAI::ContextResource::FUeAgentOperationContext& Context,
			TOptional<UE::UAI::PlanningPreflight::FChangeDraft>& OutDraft,
			FUeAgentWorldSpatialExecutionError& OutError) const override;

	private:
		FString TypeId;
		FString FamilyId;
		FString RequestKindId;
		bool bOperation = false;
		FUeAgentWorldSpatialRequestPayload Payload;
	};

	UEAGENTINTERFACE_API bool TryCreateWorldSpatialRequestDecoder(
		TSharedPtr<IUeAgentWorldSpatialRequestDecoder>& OutDecoder,
		FUeAgentWorldSpatialExecutionError& OutError);
}

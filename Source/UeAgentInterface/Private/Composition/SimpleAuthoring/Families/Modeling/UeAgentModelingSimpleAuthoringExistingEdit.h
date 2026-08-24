#pragma once

#include "CoreMinimal.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "DomainAdapters/ModelingGeometryEffect/UeAgentModelingGeometryEffectNormalObservation.h"
#include "Dom/JsonObject.h"
#include "Templates/SharedPointer.h"

class UWorld;

namespace UE::UAI::DomainAdapters::ModelingGeometryEffect
{
	class IUeAgentModelingGeometryEffectReferenceResolver;
}

namespace UE::UAI::Composition::SimpleAuthoring::Modeling::ExistingEdit
{
	struct FCarrierState
	{
		FString TargetRef;
		FString DurablePackageName;
		TOptional<StateSemantic::FUeAgentStateRevision> TargetRevision;
		FString CarrierKind;
		FGuid ActorGuid;
		FString GeometryFingerprint;
		TArray<int32> VertexIds;
		TArray<int32> EdgeIds;
		TArray<int32> BoundaryEdgeIds;
		TArray<int32> TriangleIds;
		int32 MaterialSlotCount = 0;
		TArray<FName> MaterialSlotNames;
		TArray<FString> MaterialRefs;
		TArray<int32> UvElementCounts;
		TArray<FVector2f> UvMins;
		TArray<FVector2f> UvMaxs;
		TArray<bool> UvHasElements;
		int32 NormalElementCount = 0;
		int64 NormalChecksum = 0;
		DomainAdapters::ModelingGeometryEffect::
			FNormalSemanticObservation NormalObservation;
		int32 NormalSplitEndpointCount = 0;
		FString NormalSplitTopologyFingerprint;
		bool bHasNormalSplitTopologyEvidence = false;
		TMap<int32, int32> MaterialIdByTriangle;
		int32 ConnectedComponentCount = 0;
		int32 CollisionShapeCount = 0;
		int32 CollisionBoxCount = 0;
		int32 CollisionSphereCount = 0;
		int32 CollisionCapsuleCount = 0;
		int32 CollisionConvexCount = 0;
		FBox CollisionBounds = FBox(EForceInit::ForceInit);
		int32 VertexCount = 0;
		int32 EdgeCount = 0;
		int32 BoundaryEdgeCount = 0;
		int32 BoundaryVertexCount = 0;
		FString BoundaryGeometryFingerprint;
		int32 TriangleCount = 0;
		int64 VertexChecksum = 0;
		FBox Bounds = FBox(EForceInit::ForceInit);

		bool IsValid() const;
	};

	bool TryReadCarrier(
		UWorld& World,
		DomainAdapters::ModelingGeometryEffect::
			IUeAgentModelingGeometryEffectReferenceResolver& Resolver,
		const FString& TargetRef,
		FCarrierState& OutState,
		FString& OutErrorCode,
		FString& OutErrorDetail);

	TSharedPtr<FJsonObject> BuildDraftState(
		const FCarrierState& State);

	bool TryAttachBaselineEvidence(
		UWorld& World,
		DomainAdapters::ModelingGeometryEffect::
			IUeAgentModelingGeometryEffectReferenceResolver& Resolver,
		const FString& OperationKey,
		const FCarrierState& State,
		const TSharedPtr<FJsonObject>& Payload,
		FString& OutErrorCode,
		FString& OutErrorDetail);

	bool TryReadBaseline(
		const TSharedPtr<FJsonObject>& Payload,
		FCarrierState& OutState);

	bool TryReadDurablePackageNameEvidence(
		const TSharedPtr<FJsonObject>& Payload,
		FString& OutPackageName);

	TSharedPtr<FJsonObject> BuildExpectedVerificationState(
		const FString& OperationKey,
		const FCarrierState& Baseline);

	TSharedPtr<FJsonObject> BuildActualVerificationState(
		const FString& OperationKey,
		const TSharedPtr<FJsonObject>& Payload,
		const FCarrierState& Baseline,
		const FCarrierState& Actual);
}

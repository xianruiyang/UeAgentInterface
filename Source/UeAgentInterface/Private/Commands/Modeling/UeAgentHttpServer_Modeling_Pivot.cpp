// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_Modeling.h"
#include "UeAgentHttpServer_ModelingOps.h"

#include "Module/UeAgentInterfaceLogger.h"
#include "Utilities/UeAgentJsonDiagnostics.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Components/DynamicMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMesh/DynamicMeshOverlay.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"
#include "PhysicsEngine/AggregateGeom.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshAttributes.h"
#include "UDynamicMesh.h"

namespace UeAgentModelingPivot
{
	static FString NormalizePivotPreset(const FString& InValue)
	{
		FString Value = InValue.TrimStartAndEnd().ToLower();
		Value.ReplaceInline(TEXT("-"), TEXT("_"));
		Value.ReplaceInline(TEXT(" "), TEXT("_"));
		return Value;
	}

	static bool TryGetPresetAction(const FString& Preset, FString& OutActionName)
	{
		const FString Value = NormalizePivotPreset(Preset);
		if (Value == TEXT("center") || Value == TEXT("centre"))
		{
			OutActionName = TEXT("Center");
			return true;
		}
		if (Value == TEXT("bottom"))
		{
			OutActionName = TEXT("Bottom");
			return true;
		}
		if (Value == TEXT("top"))
		{
			OutActionName = TEXT("Top");
			return true;
		}
		if (Value == TEXT("left"))
		{
			OutActionName = TEXT("Left");
			return true;
		}
		if (Value == TEXT("right"))
		{
			OutActionName = TEXT("Right");
			return true;
		}
		if (Value == TEXT("front"))
		{
			OutActionName = TEXT("Front");
			return true;
		}
		if (Value == TEXT("back"))
		{
			OutActionName = TEXT("Back");
			return true;
		}
		if (Value == TEXT("world_origin") || Value == TEXT("origin"))
		{
			OutActionName = TEXT("WorldOrigin");
			return true;
		}
		return false;
	}

	static bool IsHighLevelPivotRequest(const TSharedPtr<FJsonObject>& Params)
	{
		return Params.IsValid()
			&& (Params->HasField(TEXT("preset"))
				|| Params->HasField(TEXT("pivot_preset"))
				|| Params->HasField(TEXT("custom_point"))
				|| Params->HasField(TEXT("pivot_point"))
				|| Params->HasField(TEXT("bounds_space"))
				|| Params->HasField(TEXT("box_space"))
				|| Params->HasField(TEXT("apply_to_all_lods"))
				|| Params->HasField(TEXT("preserve_world_geometry"))
				|| Params->HasField(TEXT("asset_write_policy"))
				|| Params->HasField(TEXT("readback")));
	}

	static void AppendToolProperty(
		TSharedPtr<FJsonObject>& Params,
		const FString& PropertySet,
		const FString& PropertyPath,
		bool bValue)
	{
		if (!Params.IsValid())
		{
			Params = MakeShared<FJsonObject>();
		}

		TArray<TSharedPtr<FJsonValue>> ToolProperties;
		const TArray<TSharedPtr<FJsonValue>>* ExistingProperties = nullptr;
		if (Params->TryGetArrayField(TEXT("tool_properties"), ExistingProperties) && ExistingProperties)
		{
			ToolProperties = *ExistingProperties;
		}

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("property_set"), PropertySet);
		Entry->SetStringField(TEXT("property_path"), PropertyPath);
		Entry->SetStringField(TEXT("value_kind"), TEXT("bool"));
		TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
		Value->SetBoolField(TEXT("literal"), bValue);
		Entry->SetObjectField(TEXT("value"), Value);
		ToolProperties.Add(MakeShared<FJsonValueObject>(Entry));
		Params->SetArrayField(TEXT("tool_properties"), ToolProperties);
	}

	static bool TryGetPivotActorIds(const TSharedPtr<FJsonObject>& Params, TArray<TSharedPtr<FJsonValue>>& OutActorIds)
	{
		OutActorIds.Reset();
		if (!Params.IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* ExistingActorIds = nullptr;
		if (Params->TryGetArrayField(TEXT("actor_ids"), ExistingActorIds) && ExistingActorIds && ExistingActorIds->Num() > 0)
		{
			OutActorIds = *ExistingActorIds;
			return true;
		}

		FString ActorKey;
		Params->TryGetStringField(TEXT("actor_id"), ActorKey);
		if (ActorKey.IsEmpty())
		{
			Params->TryGetStringField(TEXT("actor_label"), ActorKey);
		}
		if (ActorKey.IsEmpty())
		{
			Params->TryGetStringField(TEXT("actor_path"), ActorKey);
		}
		if (ActorKey.IsEmpty())
		{
			Params->TryGetStringField(TEXT("actor"), ActorKey);
		}

		ActorKey = ActorKey.TrimStartAndEnd();
		if (ActorKey.IsEmpty())
		{
			return false;
		}

		OutActorIds.Add(MakeShared<FJsonValueString>(ActorKey));
		return true;
	}

	static bool TryGetVectorFromObject(const TSharedPtr<FJsonObject>& VectorObject, FVector& OutValue)
	{
		if (!VectorObject.IsValid())
		{
			return false;
		}

		double X = 0.0;
		double Y = 0.0;
		double Z = 0.0;
		if (!VectorObject->TryGetNumberField(TEXT("x"), X)
			|| !VectorObject->TryGetNumberField(TEXT("y"), Y)
			|| !VectorObject->TryGetNumberField(TEXT("z"), Z))
		{
			return false;
		}

		OutValue = FVector((float)X, (float)Y, (float)Z);
		return true;
	}

	static bool HasCustomPoint(const TSharedPtr<FJsonObject>& Params)
	{
		return Params.IsValid() && (Params->HasField(TEXT("custom_point")) || Params->HasField(TEXT("pivot_point")));
	}

	static bool TryResolveCustomPoint(
		const TSharedPtr<FJsonObject>& Params,
		const UPrimitiveComponent* Component,
		FVector& OutLocalPoint,
		FString& OutError)
	{
		const TSharedPtr<FJsonValue>* PointValue = nullptr;
		if (!Params.IsValid())
		{
			OutError = TEXT("missing_params");
			return false;
		}
		PointValue = UeAgentJsonDiagnostics::FindFieldValue(Params, TEXT("custom_point"));
		if (!PointValue)
		{
			PointValue = UeAgentJsonDiagnostics::FindFieldValue(Params, TEXT("pivot_point"));
			if (!PointValue)
			{
				OutError = TEXT("custom_point_required");
				return false;
			}
		}

		FVector Point = FVector::ZeroVector;
		if (!UeAgentJsonDiagnostics::TryReadVectorValue(*PointValue, Point))
		{
			OutError = TEXT("invalid_custom_point");
			return false;
		}

		FString Space;
		const TSharedPtr<FJsonObject> PointObject = (*PointValue).IsValid() ? (*PointValue)->AsObject() : nullptr;
		if (PointObject.IsValid())
		{
			PointObject->TryGetStringField(TEXT("space"), Space);
		}
		if (Space.IsEmpty())
		{
			Params->TryGetStringField(TEXT("custom_point_space"), Space);
		}
		if (Space.IsEmpty())
		{
			Params->TryGetStringField(TEXT("pivot_point_space"), Space);
		}
		if (Space.IsEmpty())
		{
			Space = TEXT("component_local");
		}
		Space = Space.TrimStartAndEnd().ToLower();

		if (Space == TEXT("component_local") || Space == TEXT("asset_local") || Space == TEXT("local") || Space == TEXT("mesh_local"))
		{
			OutLocalPoint = Point;
			return true;
		}
		if (Space == TEXT("world") || Space == TEXT("world_space"))
		{
			if (!Component)
			{
				OutError = TEXT("component_required_for_world_custom_point");
				return false;
			}
			OutLocalPoint = Component->GetComponentTransform().InverseTransformPosition(Point);
			return true;
		}

		OutError = TEXT("unsupported_custom_point_space");
		return false;
	}

	static bool BuildDynamicMeshLocalBounds(const UDynamicMeshComponent* Component, FBox& OutBounds)
	{
		OutBounds = FBox(ForceInit);
		if (!Component)
		{
			return false;
		}

		const UDynamicMesh* DynamicMesh = const_cast<UDynamicMeshComponent*>(Component)->GetDynamicMesh();
		if (!DynamicMesh)
		{
			return false;
		}

		DynamicMesh->ProcessMesh([&OutBounds](const UE::Geometry::FDynamicMesh3& Mesh)
		{
			for (int32 VertexId : Mesh.VertexIndicesItr())
			{
				OutBounds += FVector(Mesh.GetVertex(VertexId));
			}
		});
		return OutBounds.IsValid != 0;
	}

	static bool ResolvePresetPoint(
		const FString& Preset,
		const UPrimitiveComponent* Component,
		const FBox& LocalBounds,
		FVector& OutLocalPoint,
		FString& OutError)
	{
		if (!LocalBounds.IsValid)
		{
			OutError = TEXT("invalid_mesh_bounds");
			return false;
		}

		const FString Value = NormalizePivotPreset(Preset);
		OutLocalPoint = LocalBounds.GetCenter();
		if (Value == TEXT("center") || Value == TEXT("centre"))
		{
			return true;
		}
		if (Value == TEXT("bottom"))
		{
			OutLocalPoint.Z = LocalBounds.Min.Z;
			return true;
		}
		if (Value == TEXT("top"))
		{
			OutLocalPoint.Z = LocalBounds.Max.Z;
			return true;
		}
		if (Value == TEXT("left"))
		{
			OutLocalPoint.Y = LocalBounds.Min.Y;
			return true;
		}
		if (Value == TEXT("right"))
		{
			OutLocalPoint.Y = LocalBounds.Max.Y;
			return true;
		}
		if (Value == TEXT("front"))
		{
			OutLocalPoint.X = LocalBounds.Min.X;
			return true;
		}
		if (Value == TEXT("back"))
		{
			OutLocalPoint.X = LocalBounds.Max.X;
			return true;
		}
		if (Value == TEXT("world_origin") || Value == TEXT("origin"))
		{
			if (!Component)
			{
				OutError = TEXT("component_required_for_world_origin");
				return false;
			}
			OutLocalPoint = Component->GetComponentTransform().InverseTransformPosition(FVector::ZeroVector);
			return true;
		}

		OutError = TEXT("unsupported_pivot_preset");
		return false;
	}

	static TSharedPtr<FJsonObject> MakePivotReadbackJson(const FBox& Bounds)
	{
		TArray<TSharedPtr<FJsonValue>> Issues;
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetObjectField(TEXT("aabb"), UeAgentModelingSpatialInfo::MakeAabbJson(Bounds));
		Obj->SetObjectField(TEXT("pivot_in_aabb"), UeAgentModelingSpatialInfo::MakePivotInAabbJson(Bounds, Issues));
		if (Issues.Num() > 0)
		{
			Obj->SetArrayField(TEXT("issues"), Issues);
		}
		return Obj;
	}

	static bool ApplyDynamicMeshPivot(
		UDynamicMeshComponent* Component,
		const FVector& PivotLocal,
		TSharedPtr<FJsonObject>& OutData,
		FString& OutError)
	{
		if (!Component)
		{
			OutError = TEXT("dynamic_mesh_component_required");
			return false;
		}
		UDynamicMesh* DynamicMesh = Component->GetDynamicMesh();
		if (!DynamicMesh)
		{
			OutError = TEXT("dynamic_mesh_missing");
			return false;
		}

		FBox BeforeBounds;
		if (!BuildDynamicMeshLocalBounds(Component, BeforeBounds))
		{
			OutError = TEXT("invalid_dynamic_mesh_bounds");
			return false;
		}

		const FTransform BeforeTransform = Component->GetComponentTransform();
		const FBoxSphereBounds BeforeWorldBounds = Component->Bounds;
		const FVector NewWorldLocation = BeforeTransform.TransformPosition(PivotLocal);

		if (AActor* Owner = Component->GetOwner())
		{
			Owner->Modify();
		}
		Component->Modify();
		DynamicMesh->Modify();
		DynamicMesh->EditMesh([&PivotLocal](UE::Geometry::FDynamicMesh3& Mesh)
		{
			for (int32 VertexId : Mesh.VertexIndicesItr())
			{
				Mesh.SetVertex(VertexId, Mesh.GetVertex(VertexId) - FVector3d(PivotLocal));
			}
		}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::VertexPositions);

		Component->SetWorldLocation(NewWorldLocation, false, nullptr, ETeleportType::TeleportPhysics);
		Component->UpdateBounds();
		Component->MarkRenderStateDirty();
		if (AActor* Owner = Component->GetOwner())
		{
			Owner->MarkComponentsRenderStateDirty();
			Owner->UpdateComponentTransforms();
			Owner->MarkPackageDirty();
		}

		FBox AfterBounds;
		BuildDynamicMeshLocalBounds(Component, AfterBounds);
		const FBoxSphereBounds AfterWorldBounds = Component->Bounds;
		const double BoundsOriginDelta = FVector::Dist(BeforeWorldBounds.Origin, AfterWorldBounds.Origin);
		const double BoundsExtentDelta = FVector::Dist(BeforeWorldBounds.BoxExtent, AfterWorldBounds.BoxExtent);

		OutData->SetStringField(TEXT("pivot_backend"), TEXT("dynamic_mesh_component"));
		OutData->SetObjectField(TEXT("pivot_point_component_local"), UeAgentModelingOps::MakeVectorJson(PivotLocal));
		OutData->SetObjectField(TEXT("component_location_before"), UeAgentModelingOps::MakeVectorJson(BeforeTransform.GetLocation()));
		OutData->SetObjectField(TEXT("component_location_after"), UeAgentModelingOps::MakeVectorJson(Component->GetComponentLocation()));
		OutData->SetObjectField(TEXT("pivot_before"), MakePivotReadbackJson(BeforeBounds));
		OutData->SetObjectField(TEXT("pivot_after"), MakePivotReadbackJson(AfterBounds));
		OutData->SetBoolField(TEXT("geometry_preserved"), BoundsOriginDelta <= 0.01 && BoundsExtentDelta <= 0.01);
		OutData->SetNumberField(TEXT("world_bounds_origin_delta"), BoundsOriginDelta);
		OutData->SetNumberField(TEXT("world_bounds_extent_delta"), BoundsExtentDelta);
		OutData->SetBoolField(TEXT("collision_preserved"), true);
		OutData->SetBoolField(TEXT("accepted"), true);
		return true;
	}

	static FString NormalizePivotAssetPolicy(const TSharedPtr<FJsonObject>& Params)
	{
		FString AssetWritePolicy;
		if (Params.IsValid())
		{
			Params->TryGetStringField(TEXT("asset_write_policy"), AssetWritePolicy);
		}
		AssetWritePolicy = AssetWritePolicy.TrimStartAndEnd().ToLower();
		return AssetWritePolicy.IsEmpty() ? TEXT("require_unique_asset") : AssetWritePolicy;
	}

	static bool IsPivotWriteRequest(const TSharedPtr<FJsonObject>& Params)
	{
		return HasCustomPoint(Params) || (Params.IsValid() && (Params->HasField(TEXT("preset")) || Params->HasField(TEXT("pivot_preset"))));
	}

	static bool GetPivotPresetValue(const TSharedPtr<FJsonObject>& Params, FString& OutPreset)
	{
		OutPreset.Reset();
		if (!Params.IsValid())
		{
			return false;
		}
		if (!Params->TryGetStringField(TEXT("preset"), OutPreset))
		{
			Params->TryGetStringField(TEXT("pivot_preset"), OutPreset);
		}
		OutPreset = OutPreset.TrimStartAndEnd();
		return !OutPreset.IsEmpty();
	}

	static TArray<TSharedPtr<FJsonValue>> MakeStringArrayJson(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		for (const FString& Value : Values)
		{
			Result.Add(MakeShared<FJsonValueString>(Value));
		}
		return Result;
	}

	static bool BuildStaticMeshDescriptionBounds(const UStaticMesh* StaticMesh, FBox& OutBounds)
	{
		OutBounds = FBox(ForceInit);
		if (!StaticMesh)
		{
			return false;
		}

		const FMeshDescription* MeshDescription = StaticMesh->GetMeshDescription(0);
		if (!MeshDescription)
		{
			return false;
		}

		FStaticMeshConstAttributes Attributes(*MeshDescription);
		TVertexAttributesConstRef<FVector3f> VertexPositions = Attributes.GetVertexPositions();
		for (const FVertexID VertexID : MeshDescription->Vertices().GetElementIDs())
		{
			OutBounds += FVector(VertexPositions[VertexID]);
		}
		return OutBounds.IsValid != 0;
	}

	static FBox BuildComponentWorldAabb(const UPrimitiveComponent* Component)
	{
		if (!Component)
		{
			return FBox(ForceInit);
		}
		const FBoxSphereBounds Bounds = Component->Bounds;
		return FBox(Bounds.Origin - Bounds.BoxExtent, Bounds.Origin + Bounds.BoxExtent);
	}

	static TSharedPtr<FJsonObject> MakeWorldBoundsDeltaJson(const FBox& BeforeBounds, const FBox& AfterBounds)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!BeforeBounds.IsValid || !AfterBounds.IsValid)
		{
			Obj->SetBoolField(TEXT("valid"), false);
			return Obj;
		}

		Obj->SetBoolField(TEXT("valid"), true);
		Obj->SetNumberField(TEXT("origin_delta"), FVector::Dist(BeforeBounds.GetCenter(), AfterBounds.GetCenter()));
		Obj->SetNumberField(TEXT("extent_delta"), FVector::Dist(BeforeBounds.GetExtent(), AfterBounds.GetExtent()));
		return Obj;
	}

	static bool ResolvePresetPointWithBoundsSpace(
		const FString& Preset,
		const UPrimitiveComponent* Component,
		const FBox& LocalBounds,
		const FString& BoundsSpace,
		FVector& OutLocalPoint,
		FString& OutError)
	{
		const FString NormalizedBoundsSpace = BoundsSpace.TrimStartAndEnd().ToLower();
		if (NormalizedBoundsSpace == TEXT("world") || NormalizedBoundsSpace == TEXT("world_space"))
		{
			if (!Component)
			{
				OutError = TEXT("component_required_for_world_bounds_space");
				return false;
			}

			const FBox WorldBounds = BuildComponentWorldAabb(Component);
			FVector WorldPoint = FVector::ZeroVector;
			if (!ResolvePresetPoint(Preset, Component, WorldBounds, WorldPoint, OutError))
			{
				return false;
			}
			OutLocalPoint = Component->GetComponentTransform().InverseTransformPosition(WorldPoint);
			return true;
		}

		return ResolvePresetPoint(Preset, Component, LocalBounds, OutLocalPoint, OutError);
	}

	static bool ResolveStaticMeshPivotLocal(
		const TSharedPtr<FJsonObject>& Params,
		const UStaticMesh* StaticMesh,
		const UStaticMeshComponent* Component,
		const TSharedPtr<FJsonObject>& Normalized,
		FVector& OutPivotLocal,
		FString& OutError)
	{
		if (!StaticMesh)
		{
			OutError = TEXT("static_mesh_asset_required");
			return false;
		}

		if (HasCustomPoint(Params))
		{
			if (!TryResolveCustomPoint(Params, Component, OutPivotLocal, OutError))
			{
				return false;
			}
			if (Normalized.IsValid())
			{
				Normalized->SetStringField(TEXT("point_source"), TEXT("custom_point"));
			}
			return true;
		}

		FString Preset;
		if (!GetPivotPresetValue(Params, Preset))
		{
			OutError = TEXT("missing_pivot_preset_or_custom_point");
			return false;
		}

		FBox LocalBounds;
		if (!BuildStaticMeshDescriptionBounds(StaticMesh, LocalBounds))
		{
			OutError = TEXT("invalid_static_mesh_bounds");
			return false;
		}

		FString BoundsSpace;
		if (Normalized.IsValid())
		{
			Normalized->TryGetStringField(TEXT("bounds_space"), BoundsSpace);
		}
		if (BoundsSpace.IsEmpty() && Params.IsValid())
		{
			if (!Params->TryGetStringField(TEXT("bounds_space"), BoundsSpace))
			{
				Params->TryGetStringField(TEXT("box_space"), BoundsSpace);
			}
		}
		if (BoundsSpace.IsEmpty())
		{
			BoundsSpace = TEXT("component_local");
		}

		if (!Component && (BoundsSpace.Equals(TEXT("world"), ESearchCase::IgnoreCase) || BoundsSpace.Equals(TEXT("world_space"), ESearchCase::IgnoreCase)))
		{
			OutError = TEXT("asset_target_does_not_support_world_bounds_space");
			return false;
		}

		if (!ResolvePresetPointWithBoundsSpace(Preset, Component, LocalBounds, BoundsSpace, OutPivotLocal, OutError))
		{
			return false;
		}

		if (Normalized.IsValid())
		{
			Normalized->SetStringField(TEXT("point_source"), TEXT("preset"));
		}
		return true;
	}

	static bool TranslateStaticMeshDescriptions(
		UStaticMesh* StaticMesh,
		const FVector& PivotLocal,
		bool bApplyToAllLods,
		int32& OutLodsTouched,
		TArray<FString>& OutBuildErrors,
		FString& OutError)
	{
		OutLodsTouched = 0;
		OutBuildErrors.Reset();
		if (!StaticMesh)
		{
			OutError = TEXT("static_mesh_asset_required");
			return false;
		}

		const int32 NumSourceModels = StaticMesh->GetNumSourceModels();
		const int32 NumLodsToTouch = bApplyToAllLods ? NumSourceModels : FMath::Min(NumSourceModels, 1);
		if (NumLodsToTouch <= 0)
		{
			OutError = TEXT("static_mesh_has_no_source_lods");
			return false;
		}

		StaticMesh->Modify();
		const FVector3f PivotOffset((float)PivotLocal.X, (float)PivotLocal.Y, (float)PivotLocal.Z);
		for (int32 LodIndex = 0; LodIndex < NumLodsToTouch; ++LodIndex)
		{
			FMeshDescription* MeshDescription = StaticMesh->GetMeshDescription(LodIndex);
			if (!MeshDescription)
			{
				if (LodIndex == 0)
				{
					OutError = TEXT("static_mesh_lod0_mesh_description_missing");
					return false;
				}
				continue;
			}

			StaticMesh->ModifyMeshDescription(LodIndex);
			FStaticMeshAttributes Attributes(*MeshDescription);
			TVertexAttributesRef<FVector3f> VertexPositions = Attributes.GetVertexPositions();
			for (const FVertexID VertexID : MeshDescription->Vertices().GetElementIDs())
			{
				VertexPositions[VertexID] = VertexPositions[VertexID] - PivotOffset;
			}

			UStaticMesh::FCommitMeshDescriptionParams CommitParams;
			CommitParams.bMarkPackageDirty = true;
			StaticMesh->CommitMeshDescription(LodIndex, CommitParams);
			++OutLodsTouched;
		}

		TArray<FText> BuildErrors;
		StaticMesh->Build(false, &BuildErrors);
		for (const FText& BuildError : BuildErrors)
		{
			OutBuildErrors.Add(BuildError.ToString());
		}
		StaticMesh->PostEditChange();
		StaticMesh->MarkPackageDirty();
		return OutLodsTouched > 0;
	}

	static bool ValidateStaticMeshSimpleCollisionForPivot(const UStaticMesh* StaticMesh, FString& OutError)
	{
		const UBodySetup* BodySetup = StaticMesh ? StaticMesh->GetBodySetup() : nullptr;
		if (!BodySetup)
		{
			return true;
		}

		const FKAggregateGeom& AggGeom = BodySetup->AggGeom;
		if (AggGeom.LevelSetElems.Num() > 0
			|| AggGeom.SkinnedLevelSetElems.Num() > 0
			|| AggGeom.MLLevelSetElems.Num() > 0
			|| AggGeom.SkinnedTriangleMeshElems.Num() > 0)
		{
			OutError = TEXT("unsupported_static_mesh_collision_shape");
			return false;
		}
		return true;
	}

	static TSharedPtr<FJsonObject> TranslateStaticMeshSimpleCollision(UStaticMesh* StaticMesh, const FVector& PivotLocal)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		UBodySetup* BodySetup = StaticMesh ? StaticMesh->GetBodySetup() : nullptr;
		if (!BodySetup)
		{
			Obj->SetBoolField(TEXT("body_setup_present"), false);
			Obj->SetBoolField(TEXT("collision_touched"), false);
			return Obj;
		}

		FKAggregateGeom& AggGeom = BodySetup->AggGeom;
		const int32 ElementCount = AggGeom.GetElementCount();
		Obj->SetBoolField(TEXT("body_setup_present"), true);
		Obj->SetNumberField(TEXT("element_count"), ElementCount);
		Obj->SetNumberField(TEXT("sphere_count"), AggGeom.SphereElems.Num());
		Obj->SetNumberField(TEXT("box_count"), AggGeom.BoxElems.Num());
		Obj->SetNumberField(TEXT("sphyl_count"), AggGeom.SphylElems.Num());
		Obj->SetNumberField(TEXT("convex_count"), AggGeom.ConvexElems.Num());
		Obj->SetNumberField(TEXT("tapered_capsule_count"), AggGeom.TaperedCapsuleElems.Num());
		if (ElementCount <= 0)
		{
			Obj->SetBoolField(TEXT("collision_touched"), false);
			return Obj;
		}

		BodySetup->Modify();
		for (FKSphereElem& Elem : AggGeom.SphereElems)
		{
			Elem.Center -= PivotLocal;
		}
		for (FKBoxElem& Elem : AggGeom.BoxElems)
		{
			Elem.Center -= PivotLocal;
		}
		for (FKSphylElem& Elem : AggGeom.SphylElems)
		{
			Elem.Center -= PivotLocal;
		}
		for (FKTaperedCapsuleElem& Elem : AggGeom.TaperedCapsuleElems)
		{
			Elem.Center -= PivotLocal;
		}
		for (FKConvexElem& Elem : AggGeom.ConvexElems)
		{
			FTransform Transform = Elem.GetTransform();
			Transform.SetLocation(Transform.GetLocation() - PivotLocal);
			Elem.SetTransform(Transform);
			Elem.ResetChaosConvexMesh();
		}
		AggGeom.FreeRenderInfo();
		BodySetup->InvalidatePhysicsData();
		BodySetup->CreatePhysicsMeshes();
		BodySetup->MarkPackageDirty();
		Obj->SetBoolField(TEXT("collision_touched"), true);
		return Obj;
	}

	static void CollectStaticMeshSceneComponents(UWorld* World, const UStaticMesh* StaticMesh, TArray<UStaticMeshComponent*>& OutComponents)
	{
		OutComponents.Reset();
		if (!World || !StaticMesh)
		{
			return;
		}

		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor)
			{
				continue;
			}
			TArray<UStaticMeshComponent*> Components;
			Actor->GetComponents<UStaticMeshComponent>(Components);
			for (UStaticMeshComponent* Component : Components)
			{
				if (Component && Component->GetStaticMesh() == StaticMesh)
				{
					OutComponents.Add(Component);
				}
			}
		}
	}

	static bool MakeDefaultDuplicateAssetPath(
		const UStaticMesh* SourceMesh,
		FString& OutPackagePath,
		FString& OutAssetName,
		FString& OutError)
	{
		if (!SourceMesh || !SourceMesh->GetOutermost())
		{
			OutError = TEXT("source_static_mesh_asset_invalid");
			return false;
		}

		FString BasePackageName;
		const FString SourcePackageName = SourceMesh->GetOutermost()->GetName();
		if (SourcePackageName.StartsWith(TEXT("/Game/")))
		{
			BasePackageName = SourcePackageName + TEXT("_Pivot");
		}
		else
		{
			BasePackageName = FString::Printf(TEXT("/Game/UAI/Pivot/%s_Pivot"), *SourceMesh->GetName());
		}
		FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
		FString UniquePackageName;
		FString UniqueAssetName;
		AssetToolsModule.Get().CreateUniqueAssetName(BasePackageName, TEXT(""), UniquePackageName, UniqueAssetName);
		OutPackagePath = FPackageName::GetLongPackagePath(UniquePackageName);
		OutAssetName = UniqueAssetName;
		if (OutPackagePath.IsEmpty() || OutAssetName.IsEmpty())
		{
			OutError = TEXT("duplicate_asset_path_resolution_failed");
			return false;
		}
		return true;
	}

	static bool ResolveRequestedDuplicateAssetPath(
		const TSharedPtr<FJsonObject>& Params,
		const UStaticMesh* SourceMesh,
		FString& OutPackagePath,
		FString& OutAssetName,
		FString& OutError)
	{
		FString DestinationPath;
		if (Params.IsValid())
		{
			if (!Params->TryGetStringField(TEXT("duplicate_asset_path"), DestinationPath))
			{
				Params->TryGetStringField(TEXT("destination_asset_path"), DestinationPath);
			}
		}
		DestinationPath = DestinationPath.TrimStartAndEnd();
		if (DestinationPath.IsEmpty())
		{
			return MakeDefaultDuplicateAssetPath(SourceMesh, OutPackagePath, OutAssetName, OutError);
		}

		if (DestinationPath.Contains(TEXT(".")))
		{
			FString PackageOnly;
			DestinationPath.Split(TEXT("."), &PackageOnly, nullptr, ESearchCase::IgnoreCase, ESearchDir::FromStart);
			DestinationPath = PackageOnly;
		}
		if (!FPackageName::IsValidLongPackageName(DestinationPath))
		{
			OutError = TEXT("invalid_duplicate_asset_path");
			return false;
		}

		OutPackagePath = FPackageName::GetLongPackagePath(DestinationPath);
		OutAssetName = FPackageName::GetLongPackageAssetName(DestinationPath);
		if (OutPackagePath.IsEmpty() || OutAssetName.IsEmpty())
		{
			OutError = TEXT("invalid_duplicate_asset_path");
			return false;
		}
		if (UeAgentModelingOps::LoadAssetObject(DestinationPath))
		{
			OutError = TEXT("duplicate_asset_destination_exists");
			return false;
		}
		return true;
	}

	static UStaticMesh* DuplicateStaticMeshForPivot(
		const TSharedPtr<FJsonObject>& Params,
		UStaticMesh* SourceMesh,
		TSharedPtr<FJsonObject>& OutDuplicateJson,
		FString& OutError)
	{
		OutDuplicateJson = MakeShared<FJsonObject>();
		if (!SourceMesh)
		{
			OutError = TEXT("source_static_mesh_asset_invalid");
			return nullptr;
		}

		FString DestinationPackagePath;
		FString DestinationAssetName;
		if (!ResolveRequestedDuplicateAssetPath(Params, SourceMesh, DestinationPackagePath, DestinationAssetName, OutError))
		{
			return nullptr;
		}

		FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
		UObject* DuplicatedObject = AssetToolsModule.Get().DuplicateAsset(DestinationAssetName, DestinationPackagePath, SourceMesh);
		UStaticMesh* DuplicatedMesh = Cast<UStaticMesh>(DuplicatedObject);
		if (!DuplicatedMesh)
		{
			OutError = TEXT("duplicate_static_mesh_failed");
			return nullptr;
		}

		FAssetRegistryModule::AssetCreated(DuplicatedMesh);
		DuplicatedMesh->MarkPackageDirty();
		OutDuplicateJson->SetStringField(TEXT("source_asset_path"), SourceMesh->GetOutermost() ? SourceMesh->GetOutermost()->GetName() : FString());
		OutDuplicateJson->SetStringField(TEXT("asset_path"), DuplicatedMesh->GetOutermost() ? DuplicatedMesh->GetOutermost()->GetName() : FString());
		OutDuplicateJson->SetStringField(TEXT("object_path"), DuplicatedMesh->GetPathName());
		return DuplicatedMesh;
	}

	static void MoveComponentsToPreserveStaticMeshWorldGeometry(const TArray<UStaticMeshComponent*>& Components, const FVector& PivotLocal, TArray<TSharedPtr<FJsonValue>>& OutComponentJson)
	{
		OutComponentJson.Reset();
		for (UStaticMeshComponent* Component : Components)
		{
			if (!Component)
			{
				continue;
			}

			AActor* Owner = Component->GetOwner();
			const FTransform BeforeTransform = Component->GetComponentTransform();
			const FBox BeforeWorldBounds = BuildComponentWorldAabb(Component);
			const FVector NewWorldLocation = BeforeTransform.TransformPosition(PivotLocal);
			if (Owner)
			{
				Owner->Modify();
			}
			Component->Modify();
			Component->SetWorldLocation(NewWorldLocation, false, nullptr, ETeleportType::TeleportPhysics);
			Component->UpdateBounds();
			Component->MarkRenderStateDirty();
			if (Owner)
			{
				Owner->MarkComponentsRenderStateDirty();
				Owner->UpdateComponentTransforms();
				Owner->MarkPackageDirty();
			}

			const FBox AfterWorldBounds = BuildComponentWorldAabb(Component);
			TSharedPtr<FJsonObject> ComponentObj = MakeShared<FJsonObject>();
			ComponentObj->SetObjectField(TEXT("target"), UeAgentModelingSpatialInfo::MakeComponentTargetJson(Owner, Component, TEXT("static_mesh_component")));
			ComponentObj->SetObjectField(TEXT("location_before"), UeAgentModelingOps::MakeVectorJson(BeforeTransform.GetLocation()));
			ComponentObj->SetObjectField(TEXT("location_after"), UeAgentModelingOps::MakeVectorJson(Component->GetComponentLocation()));
			ComponentObj->SetObjectField(TEXT("world_bounds_delta"), MakeWorldBoundsDeltaJson(BeforeWorldBounds, AfterWorldBounds));
			OutComponentJson.Add(MakeShared<FJsonValueObject>(ComponentObj));
		}
	}

	static bool ApplyActorEditorPivotOffset(
		const TSharedPtr<FJsonObject>& Params,
		const TSharedPtr<FJsonObject>& Normalized,
		TSharedPtr<FJsonObject>& OutData,
		FString& OutError)
	{
		if (Params.IsValid() && Params->HasField(TEXT("asset_path")))
		{
			OutError = TEXT("editor_pivot_offset_requires_actor_target");
			return false;
		}

		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			OutError = TEXT("editor_world_not_available");
			return false;
		}

		AActor* Actor = nullptr;
		FString ResolveError;
		if (!UeAgentModelingSpatialInfo::ResolveActorFromParams(World, Params, Actor, ResolveError))
		{
			OutError = ResolveError;
			return false;
		}

		FString ComponentName;
		if (Params.IsValid() && !Params->TryGetStringField(TEXT("component_name"), ComponentName))
		{
			Params->TryGetStringField(TEXT("component"), ComponentName);
		}

		UPrimitiveComponent* PrimitiveComponent = nullptr;
		if (!UeAgentModelingSpatialInfo::ResolveMeshComponent(Actor, ComponentName, PrimitiveComponent, ResolveError))
		{
			OutError = ResolveError;
			return false;
		}

		FBox LocalBounds(ForceInit);
		if (const UStaticMeshComponent* StaticMeshComponent = Cast<UStaticMeshComponent>(PrimitiveComponent))
		{
			if (!BuildStaticMeshDescriptionBounds(StaticMeshComponent->GetStaticMesh(), LocalBounds))
			{
				OutError = TEXT("invalid_static_mesh_bounds");
				return false;
			}
		}
		else if (const UDynamicMeshComponent* DynamicMeshComponent = Cast<UDynamicMeshComponent>(PrimitiveComponent))
		{
			if (!BuildDynamicMeshLocalBounds(DynamicMeshComponent, LocalBounds))
			{
				OutError = TEXT("invalid_dynamic_mesh_bounds");
				return false;
			}
		}
		else
		{
			const FBoxSphereBounds Bounds = PrimitiveComponent->CalcBounds(FTransform::Identity);
			LocalBounds = FBox(Bounds.Origin - Bounds.BoxExtent, Bounds.Origin + Bounds.BoxExtent);
		}

		FVector PivotLocal = FVector::ZeroVector;
		if (HasCustomPoint(Params))
		{
			if (!TryResolveCustomPoint(Params, PrimitiveComponent, PivotLocal, OutError))
			{
				return false;
			}
			if (Normalized.IsValid())
			{
				Normalized->SetStringField(TEXT("point_source"), TEXT("custom_point"));
			}
		}
		else
		{
			FString Preset;
			if (!GetPivotPresetValue(Params, Preset))
			{
				OutError = TEXT("missing_pivot_preset_or_custom_point");
				return false;
			}

			FString BoundsSpace;
			if (Normalized.IsValid())
			{
				Normalized->TryGetStringField(TEXT("bounds_space"), BoundsSpace);
			}
			if (!ResolvePresetPointWithBoundsSpace(Preset, PrimitiveComponent, LocalBounds, BoundsSpace, PivotLocal, OutError))
			{
				return false;
			}
			if (Normalized.IsValid())
			{
				Normalized->SetStringField(TEXT("point_source"), TEXT("preset"));
			}
		}

		const FVector PivotWorld = PrimitiveComponent->GetComponentTransform().TransformPosition(PivotLocal);
		const FVector PivotOffsetActorLocal = Actor->GetTransform().InverseTransformVector(PivotWorld - Actor->GetActorLocation());
		Actor->Modify();
		Actor->SetPivotOffset(PivotOffsetActorLocal);
		Actor->MarkPackageDirty();

		OutData->SetStringField(TEXT("pivot_backend"), TEXT("actor_editor_pivot_offset"));
		OutData->SetObjectField(TEXT("normalized_request"), Normalized);
		OutData->SetObjectField(TEXT("target"), UeAgentModelingSpatialInfo::MakeComponentTargetJson(Actor, PrimitiveComponent, UeAgentModelingSpatialInfo::GetComponentTargetType(PrimitiveComponent)));
		OutData->SetObjectField(TEXT("frame"), UeAgentModelingSpatialInfo::MakeFrameJson(TEXT("component_local")));
		OutData->SetObjectField(TEXT("pivot_point_component_local"), UeAgentModelingOps::MakeVectorJson(PivotLocal));
		OutData->SetObjectField(TEXT("pivot_point_world"), UeAgentModelingOps::MakeVectorJson(PivotWorld));
		OutData->SetObjectField(TEXT("pivot_offset_actor_local"), UeAgentModelingOps::MakeVectorJson(PivotOffsetActorLocal));
		OutData->SetBoolField(TEXT("mesh_geometry_changed"), false);
		OutData->SetBoolField(TEXT("collision_preserved"), true);
		OutData->SetBoolField(TEXT("accepted"), true);
		return true;
	}

	static bool ApplyStaticMeshPivot(
		const TSharedPtr<FJsonObject>& Params,
		const TSharedPtr<FJsonObject>& Normalized,
		UStaticMesh* SourceMesh,
		UStaticMeshComponent* TargetComponent,
		AActor* TargetActor,
		UWorld* World,
		TSharedPtr<FJsonObject>& OutData,
		FString& OutError)
	{
		if (!SourceMesh)
		{
			OutError = TEXT("static_mesh_asset_required");
			return false;
		}
		if (!ValidateStaticMeshSimpleCollisionForPivot(SourceMesh, OutError))
		{
			return false;
		}

		FBox SourceBounds;
		if (!BuildStaticMeshDescriptionBounds(SourceMesh, SourceBounds))
		{
			OutError = TEXT("invalid_static_mesh_bounds");
			return false;
		}

		FVector PivotLocal = FVector::ZeroVector;
		if (!ResolveStaticMeshPivotLocal(Params, SourceMesh, TargetComponent, Normalized, PivotLocal, OutError))
		{
			return false;
		}

		const FString Policy = NormalizePivotAssetPolicy(Params);
		TArray<UStaticMeshComponent*> OriginalSceneComponents;
		CollectStaticMeshSceneComponents(World, SourceMesh, OriginalSceneComponents);
		const bool bHasSharedOpenWorldRefs = OriginalSceneComponents.Num() > 1;
		bool bDuplicated = false;
		TSharedPtr<FJsonObject> DuplicateJson;
		UStaticMesh* MeshToEdit = SourceMesh;
		TArray<UStaticMeshComponent*> ComponentsToMove;

		if (Policy == TEXT("require_unique_asset") && bHasSharedOpenWorldRefs)
		{
			OutError = TEXT("static_mesh_asset_is_shared");
			OutData->SetNumberField(TEXT("open_world_reference_count"), OriginalSceneComponents.Num());
			return false;
		}
		if (Policy == TEXT("duplicate_if_shared") && bHasSharedOpenWorldRefs)
		{
			MeshToEdit = DuplicateStaticMeshForPivot(Params, SourceMesh, DuplicateJson, OutError);
			if (!MeshToEdit)
			{
				return false;
			}
			bDuplicated = true;
			if (TargetComponent)
			{
				TargetComponent->Modify();
				TargetComponent->SetStaticMesh(MeshToEdit);
				TargetComponent->UpdateBounds();
				TargetComponent->MarkRenderStateDirty();
				ComponentsToMove.Add(TargetComponent);
			}
		}
		else
		{
			ComponentsToMove = OriginalSceneComponents;
		}

		if (!ValidateStaticMeshSimpleCollisionForPivot(MeshToEdit, OutError))
		{
			return false;
		}

		FBox EditBeforeBounds;
		if (!BuildStaticMeshDescriptionBounds(MeshToEdit, EditBeforeBounds))
		{
			OutError = TEXT("invalid_static_mesh_bounds");
			return false;
		}

		bool bApplyToAllLods = true;
		if (Params.IsValid())
		{
			Params->TryGetBoolField(TEXT("apply_to_all_lods"), bApplyToAllLods);
		}

		TSharedPtr<FJsonObject> CollisionJson = TranslateStaticMeshSimpleCollision(MeshToEdit, PivotLocal);
		int32 LodsTouched = 0;
		TArray<FString> BuildErrors;
		if (!TranslateStaticMeshDescriptions(MeshToEdit, PivotLocal, bApplyToAllLods, LodsTouched, BuildErrors, OutError))
		{
			return false;
		}

		TArray<TSharedPtr<FJsonValue>> ComponentMoveJson;
		MoveComponentsToPreserveStaticMeshWorldGeometry(ComponentsToMove, PivotLocal, ComponentMoveJson);

		FBox AfterBounds;
		BuildStaticMeshDescriptionBounds(MeshToEdit, AfterBounds);

		OutData->SetStringField(TEXT("pivot_backend"), TEXT("static_mesh_asset"));
		OutData->SetObjectField(TEXT("normalized_request"), Normalized);
		if (TargetComponent)
		{
			OutData->SetObjectField(TEXT("target"), UeAgentModelingSpatialInfo::MakeComponentTargetJson(TargetActor, TargetComponent, TEXT("static_mesh_component")));
		}
		else
		{
			OutData->SetObjectField(TEXT("target"), UeAgentModelingSpatialInfo::MakeAssetTargetJson(MeshToEdit, TEXT("static_mesh_asset")));
		}
		OutData->SetObjectField(TEXT("frame"), UeAgentModelingSpatialInfo::MakeFrameJson(TargetComponent ? TEXT("component_local") : TEXT("asset_local")));
		OutData->SetStringField(TEXT("asset_write_policy"), Policy);
		OutData->SetStringField(TEXT("source_asset_path"), SourceMesh->GetOutermost() ? SourceMesh->GetOutermost()->GetName() : FString());
		OutData->SetStringField(TEXT("edited_asset_path"), MeshToEdit->GetOutermost() ? MeshToEdit->GetOutermost()->GetName() : FString());
		OutData->SetBoolField(TEXT("asset_duplicated"), bDuplicated);
		if (DuplicateJson.IsValid())
		{
			OutData->SetObjectField(TEXT("duplicate_asset"), DuplicateJson);
		}
		OutData->SetObjectField(TEXT("pivot_point_asset_local"), UeAgentModelingOps::MakeVectorJson(PivotLocal));
		OutData->SetObjectField(TEXT("pivot_before"), MakePivotReadbackJson(EditBeforeBounds));
		OutData->SetObjectField(TEXT("pivot_after"), MakePivotReadbackJson(AfterBounds));
		OutData->SetNumberField(TEXT("open_world_reference_count"), OriginalSceneComponents.Num());
		OutData->SetBoolField(TEXT("shared_asset_modified"), !bDuplicated && bHasSharedOpenWorldRefs);
		OutData->SetNumberField(TEXT("affected_component_count"), ComponentsToMove.Num());
		OutData->SetArrayField(TEXT("affected_components"), ComponentMoveJson);
		OutData->SetObjectField(TEXT("collision"), CollisionJson);
		OutData->SetBoolField(TEXT("collision_preserved"), true);
		OutData->SetNumberField(TEXT("lods_touched"), LodsTouched);
		if (BuildErrors.Num() > 0)
		{
			OutData->SetArrayField(TEXT("build_errors"), MakeStringArrayJson(BuildErrors));
		}
		OutData->SetBoolField(TEXT("accepted"), true);
		return true;
	}

	static bool ShouldUseDynamicMeshBackend(const TSharedPtr<FJsonObject>& Params)
	{
		return HasCustomPoint(Params) || (Params.IsValid() && (Params->HasField(TEXT("preset")) || Params->HasField(TEXT("pivot_preset"))));
	}

	static bool NormalizeEditPivotWrapperParams(
		TSharedPtr<FJsonObject>& Params,
		TSharedPtr<FJsonObject>& OutNormalized,
		FString& OutError)
	{
		OutNormalized = MakeShared<FJsonObject>();
		bool bHasPreset = false;
		FString Preset;
		if (Params.IsValid())
		{
			bHasPreset = Params->TryGetStringField(TEXT("preset"), Preset);
			if (!bHasPreset)
			{
				bHasPreset = Params->TryGetStringField(TEXT("pivot_preset"), Preset);
			}
		}

		if (bHasPreset)
		{
			FString ActionName;
			if (!TryGetPresetAction(Preset, ActionName))
			{
				OutError = TEXT("unsupported_pivot_preset");
				return false;
			}
			FString ExistingPostAction;
			if (Params->TryGetStringField(TEXT("post_action"), ExistingPostAction)
				&& !ExistingPostAction.IsEmpty()
				&& !ExistingPostAction.Equals(ActionName, ESearchCase::IgnoreCase))
			{
				OutError = TEXT("conflicting_pivot_post_action");
				return false;
			}
			Params->SetStringField(TEXT("post_action"), ActionName);
			OutNormalized->SetStringField(TEXT("preset"), NormalizePivotPreset(Preset));
			OutNormalized->SetStringField(TEXT("post_action"), ActionName);
			if (!Params->HasField(TEXT("accept")))
			{
				Params->SetBoolField(TEXT("accept"), true);
			}
		}

		FString BoundsSpace;
		if (Params.IsValid() && !Params->TryGetStringField(TEXT("bounds_space"), BoundsSpace))
		{
			Params->TryGetStringField(TEXT("box_space"), BoundsSpace);
		}
		BoundsSpace = BoundsSpace.TrimStartAndEnd().ToLower();
		if (!BoundsSpace.IsEmpty())
		{
			if (BoundsSpace == TEXT("world") || BoundsSpace == TEXT("world_space"))
			{
				AppendToolProperty(Params, TEXT("UEditPivotToolActionPropertySet"), TEXT("bUseWorldBox"), true);
				OutNormalized->SetStringField(TEXT("bounds_space"), TEXT("world"));
			}
			else if (BoundsSpace == TEXT("object") || BoundsSpace == TEXT("object_local") || BoundsSpace == TEXT("component_local") || BoundsSpace == TEXT("asset_local") || BoundsSpace == TEXT("local"))
			{
				AppendToolProperty(Params, TEXT("UEditPivotToolActionPropertySet"), TEXT("bUseWorldBox"), false);
				OutNormalized->SetStringField(TEXT("bounds_space"), BoundsSpace == TEXT("asset_local") ? TEXT("asset_local") : TEXT("component_local"));
			}
			else
			{
				OutError = TEXT("unsupported_bounds_space");
				return false;
			}
		}

		bool bApplyToAllLods = true;
		if (Params.IsValid() && Params->TryGetBoolField(TEXT("apply_to_all_lods"), bApplyToAllLods))
		{
			AppendToolProperty(Params, TEXT("UEditPivotToolProperties"), TEXT("bApplyToAllLODs"), bApplyToAllLods);
			OutNormalized->SetBoolField(TEXT("apply_to_all_lods"), bApplyToAllLods);
		}

		bool bPreserveWorldGeometry = true;
		if (Params.IsValid() && Params->TryGetBoolField(TEXT("preserve_world_geometry"), bPreserveWorldGeometry) && !bPreserveWorldGeometry)
		{
			OutError = TEXT("unsupported_preserve_world_geometry_false");
			return false;
		}
		OutNormalized->SetBoolField(TEXT("preserve_world_geometry"), true);

		FString AssetWritePolicy;
		if (Params.IsValid() && Params->TryGetStringField(TEXT("asset_write_policy"), AssetWritePolicy))
		{
			const FString Policy = AssetWritePolicy.TrimStartAndEnd().ToLower();
			if (!Policy.IsEmpty()
				&& Policy != TEXT("require_unique_asset")
				&& Policy != TEXT("duplicate_if_shared")
				&& Policy != TEXT("editor_pivot_offset_only")
				&& Policy != TEXT("allow_shared_asset")
				&& Policy != TEXT("allow_selected_assets"))
			{
				OutError = TEXT("unsupported_asset_write_policy");
				return false;
			}
			OutNormalized->SetStringField(TEXT("asset_write_policy"), Policy.IsEmpty() ? TEXT("require_unique_asset") : Policy);
		}
		else
		{
			OutNormalized->SetStringField(TEXT("asset_write_policy"), TEXT("require_unique_asset"));
		}

		return true;
	}
}

bool FUeAgentLegacyMechanics::CmdModelingSetPivot(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentModelingPivot;
	using namespace UeAgentModelingSpatialInfo;

	OutData = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> EffectiveParams = Ctx.Params.IsValid() ? MakeShared<FJsonObject>(*Ctx.Params) : MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> Normalized;
	if (!NormalizeEditPivotWrapperParams(EffectiveParams, Normalized, OutError))
	{
		return false;
	}

	bool bReadback = false;
	EffectiveParams->TryGetBoolField(TEXT("readback"), bReadback);

	const FString AssetWritePolicy = NormalizePivotAssetPolicy(EffectiveParams);
	if (AssetWritePolicy == TEXT("editor_pivot_offset_only"))
	{
		return ApplyActorEditorPivotOffset(EffectiveParams, Normalized, OutData, OutError);
	}

	UWorld* World = nullptr;
	if (IsPivotWriteRequest(EffectiveParams))
	{
		FString AssetPath;
		EffectiveParams->TryGetStringField(TEXT("asset_path"), AssetPath);
		AssetPath = AssetPath.TrimStartAndEnd();
		FString WorldError;
		World = GetEditorWorld(WorldError);
		if (!AssetPath.IsEmpty())
		{
			UStaticMesh* StaticMesh = UeAgentModelingOps::LoadStaticMeshAsset(AssetPath);
			if (!StaticMesh)
			{
				OutError = TEXT("static_mesh_asset_not_found");
				return false;
			}
			OutData->SetObjectField(TEXT("normalized_request"), Normalized);
			return ApplyStaticMeshPivot(EffectiveParams, Normalized, StaticMesh, nullptr, nullptr, World, OutData, OutError);
		}

		if (World)
		{
			AActor* Actor = nullptr;
			FString ResolveError;
			if (ResolveActorFromParams(World, EffectiveParams, Actor, ResolveError))
			{
				FString ComponentName;
				if (!EffectiveParams->TryGetStringField(TEXT("component_name"), ComponentName))
				{
					EffectiveParams->TryGetStringField(TEXT("component"), ComponentName);
				}
				UPrimitiveComponent* PrimitiveComponent = nullptr;
				if (ResolveMeshComponent(Actor, ComponentName, PrimitiveComponent, ResolveError))
				{
					if (UDynamicMeshComponent* DynamicMeshComponent = Cast<UDynamicMeshComponent>(PrimitiveComponent))
					{
						FString VoxelGridId;
						FBox LocalBounds;
						if (!BuildDynamicMeshLocalBounds(DynamicMeshComponent, LocalBounds))
						{
							OutError = TEXT("invalid_dynamic_mesh_bounds");
							return false;
						}

						FVector PivotLocal = FVector::ZeroVector;
						if (HasCustomPoint(EffectiveParams))
						{
							if (!TryResolveCustomPoint(EffectiveParams, DynamicMeshComponent, PivotLocal, OutError))
							{
								return false;
							}
							Normalized->SetStringField(TEXT("point_source"), TEXT("custom_point"));
						}
						else
						{
							FString Preset;
							if (!EffectiveParams->TryGetStringField(TEXT("preset"), Preset))
							{
								EffectiveParams->TryGetStringField(TEXT("pivot_preset"), Preset);
							}
							if (!ResolvePresetPoint(Preset, DynamicMeshComponent, LocalBounds, PivotLocal, OutError))
							{
								return false;
							}
							Normalized->SetStringField(TEXT("point_source"), TEXT("preset"));
						}

						OutData->SetObjectField(TEXT("normalized_request"), Normalized);
						OutData->SetObjectField(TEXT("target"), MakeComponentTargetJson(Actor, DynamicMeshComponent, TEXT("dynamic_mesh_component")));
						OutData->SetObjectField(TEXT("frame"), MakeFrameJson(TEXT("component_local")));
						if (!ApplyDynamicMeshPivot(DynamicMeshComponent, PivotLocal, OutData, OutError))
						{
							return false;
						}
						return true;
					}
					if (UStaticMeshComponent* StaticMeshComponent = Cast<UStaticMeshComponent>(PrimitiveComponent))
					{
						UStaticMesh* StaticMesh = StaticMeshComponent->GetStaticMesh();
						if (!StaticMesh)
						{
							OutError = TEXT("static_mesh_component_has_no_mesh");
							return false;
						}
						OutData->SetObjectField(TEXT("normalized_request"), Normalized);
						return ApplyStaticMeshPivot(EffectiveParams, Normalized, StaticMesh, StaticMeshComponent, Actor, World, OutData, OutError);
					}
				}
			}
		}

		if (HasCustomPoint(EffectiveParams))
		{
			OutError = TEXT("custom_point_requires_supported_mesh_write_backend");
			return false;
		}
	}

	TArray<TSharedPtr<FJsonValue>> ActorIds;
	if (TryGetPivotActorIds(EffectiveParams, ActorIds))
	{
		TSharedPtr<FJsonObject> SelectionParams = MakeShared<FJsonObject>();
		SelectionParams->SetArrayField(TEXT("actor_ids"), ActorIds);
		SelectionParams->SetBoolField(TEXT("sync_geometry_targets"), true);
		FUeAgentRequestContext SelectionCtx = Ctx;
		SelectionCtx.Params = SelectionParams;
		TSharedPtr<FJsonObject> SelectionData;
		if (!CmdModelingSetSelection(SelectionCtx, SelectionData, OutError))
		{
			return false;
		}
		OutData->SetObjectField(TEXT("selection"), SelectionData);
	}

	FUeAgentRequestContext WrappedCtx = Ctx;
	WrappedCtx.Params = EffectiveParams;
	TSharedPtr<FJsonObject> ToolData;
	if (!CmdModelingStartTool(WrappedCtx, ToolData, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("pivot_backend"), TEXT("edit_pivot_tool"));
	OutData->SetObjectField(TEXT("normalized_request"), Normalized);
	OutData->SetObjectField(TEXT("tool"), ToolData);
	if (ToolData.IsValid() && ToolData->HasField(TEXT("accepted")))
	{
		OutData->SetBoolField(TEXT("accepted"), ToolData->GetBoolField(TEXT("accepted")));
	}

	if (bReadback && ActorIds.Num() == 1)
	{
		TSharedPtr<FJsonObject> ReadbackParams = MakeShared<FJsonObject>();
		ReadbackParams->SetStringField(TEXT("actor_id"), ActorIds[0]->AsString());
		FString ComponentName;
		if (EffectiveParams->TryGetStringField(TEXT("component_name"), ComponentName) || EffectiveParams->TryGetStringField(TEXT("component"), ComponentName))
		{
			ReadbackParams->SetStringField(TEXT("component_name"), ComponentName);
		}
		FUeAgentRequestContext ReadbackCtx = Ctx;
		ReadbackCtx.Params = ReadbackParams;
		TSharedPtr<FJsonObject> ReadbackData;
		FString ReadbackError;
		if (CmdModelingGetMeshSpatialInfo(ReadbackCtx, ReadbackData, ReadbackError))
		{
			OutData->SetObjectField(TEXT("readback"), ReadbackData);
		}
		else
		{
			OutData->SetStringField(TEXT("readback_error"), ReadbackError);
		}
	}

	FUeAgentInterfaceLogger::Log(TEXT("ModelingSetPivot req=%s backend=%s"), *Ctx.RequestId, TEXT("edit_pivot_tool"));
	return true;
}

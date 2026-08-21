// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_Level.h"

#include "UeAgentHttpServer_LevelOps.h"

#include "Utilities/UeAgentJsonDiagnostics.h"

#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/DynamicMeshComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SphereComponent.h"
#include "Components/SplineMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GeometryCacheComponent.h"
#include "PhysicsEngine/AggregateGeom.h"
#include "PhysicsEngine/BodySetup.h"

namespace UeAgentLevelMeshShapes
{
	static constexpr const TCHAR* VoxelCollisionTagPrefix = TEXT("UAIEditableVoxelGridCollision:");

	enum class EQueryShapeType : uint8
	{
		Aabb,
		Sphere,
		Obb,
	};

	struct FObb
	{
		FVector Center = FVector::ZeroVector;
		FVector Axis[3] = { FVector::ForwardVector, FVector::RightVector, FVector::UpVector };
		FVector Half = FVector::ZeroVector;
	};

	struct FQuery
	{
		EQueryShapeType Type = EQueryShapeType::Aabb;
		FBox Aabb = FBox(EForceInit::ForceInit);
		FVector Center = FVector::ZeroVector;
		double Radius = 0.0;
		FObb Obb;
	};

	struct FFilters
	{
		FString FolderPathPrefix;
		FString IgnoreFolderPathPrefix;
		TArray<FString> AcceptTags;
		TArray<FString> IgnoreTags;
		TSet<FString> Types;
	};

	struct FOutputOptions
	{
		bool bStandard = false;
		bool bDebug = false;
	};

	static TSharedPtr<FJsonValue> Vec3Value(const FVector& Value)
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		Items.Reserve(3);
		Items.Add(MakeShared<FJsonValueNumber>(Value.X));
		Items.Add(MakeShared<FJsonValueNumber>(Value.Y));
		Items.Add(MakeShared<FJsonValueNumber>(Value.Z));
		return MakeShared<FJsonValueArray>(Items);
	}

	static TSharedPtr<FJsonObject> CompactTransformJson(const FTransform& Transform)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetField(TEXT("t"), Vec3Value(Transform.GetLocation()));
		const FRotator Rotation = Transform.Rotator();
		Obj->SetField(TEXT("r"), Vec3Value(FVector(Rotation.Pitch, Rotation.Yaw, Rotation.Roll)));
		Obj->SetField(TEXT("s"), Vec3Value(Transform.GetScale3D()));
		return Obj;
	}

	static TSharedPtr<FJsonObject> CompactBoundsJson(const FBox& Box)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetField(TEXT("c"), Vec3Value(Box.GetCenter()));
		Obj->SetField(TEXT("e"), Vec3Value(Box.GetExtent()));
		return Obj;
	}

	static FBox TransformLocalAabbToWorldAabb(const FBox& LocalBox, const FTransform& OwnerToWorld)
	{
		FBox WorldBox(EForceInit::ForceInit);
		if (!LocalBox.IsValid)
		{
			return WorldBox;
		}

		const FVector Center = LocalBox.GetCenter();
		const FVector Extent = LocalBox.GetExtent();
		for (int32 SignX = -1; SignX <= 1; SignX += 2)
		{
			for (int32 SignY = -1; SignY <= 1; SignY += 2)
			{
				for (int32 SignZ = -1; SignZ <= 1; SignZ += 2)
				{
					const FVector LocalCorner = Center + FVector(Extent.X * SignX, Extent.Y * SignY, Extent.Z * SignZ);
					WorldBox += OwnerToWorld.TransformPosition(LocalCorner);
				}
			}
		}
		return WorldBox;
	}

	static void SplitSourceAuthority(const FString& Src, FString& OutSource, FString& OutAuthority)
	{
		FString Left;
		FString Right;
		if (Src.Split(TEXT("/"), &Left, &Right))
		{
			OutSource = Left;
			OutAuthority = Right;
			return;
		}
		OutSource = Src;
		OutAuthority = TEXT("");
	}

	static TSharedPtr<FJsonObject> ShapeSpaceJson()
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("frame"), TEXT("owner_local"));
		Obj->SetStringField(TEXT("unit"), TEXT("cm"));
		Obj->SetStringField(TEXT("repr"), TEXT("aabb"));
		return Obj;
	}

	static bool TryGetVectorField(const TSharedPtr<FJsonObject>& Obj, const FString& Key, FVector& OutValue)
	{
		const TSharedPtr<FJsonValue>* FieldValue = UeAgentJsonDiagnostics::FindFieldValue(Obj, Key);
		return FieldValue && UeAgentJsonDiagnostics::TryReadVectorValue(*FieldValue, OutValue);
	}

	static bool TryGetStringField(const TSharedPtr<FJsonObject>& Obj, const FString& Key, FString& OutValue)
	{
		return Obj.IsValid() && Obj->TryGetStringField(Key, OutValue);
	}

	static bool TryGetNumberField(const TSharedPtr<FJsonObject>& Obj, const FString& Key, double& OutValue)
	{
		return UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Obj, { Key }, OutValue);
	}

	static bool TryGetBoolField(const TSharedPtr<FJsonObject>& Obj, const FString& Key, bool& OutValue)
	{
		return Obj.IsValid() && Obj->TryGetBoolField(Key, OutValue);
	}

	static void ParseStringArray(const TSharedPtr<FJsonObject>& Obj, const FString& Key, TArray<FString>& OutStrings)
	{
		OutStrings.Reset();
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Obj.IsValid() || !Obj->TryGetArrayField(Key, Values) || !Values)
		{
			return;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			const FString Text = Value.IsValid() ? Value->AsString().TrimStartAndEnd() : FString();
			if (!Text.IsEmpty())
			{
				OutStrings.Add(Text);
			}
		}
	}

	static FString NormalizeTypeToken(const FString& InType)
	{
		const FString Trimmed = InType.TrimStartAndEnd();
		FString Out;
		Out.Reserve(Trimmed.Len());
		for (TCHAR Ch : Trimmed)
		{
			if (FChar::IsAlnum(Ch))
			{
				Out.AppendChar(FChar::ToLower(Ch));
			}
			else if ((Ch == TCHAR('_') || Ch == TCHAR('-') || Ch == TCHAR(' ')) && Out.Len() > 0 && !Out.EndsWith(TEXT("_")))
			{
				Out.AppendChar(TCHAR('_'));
			}
		}
		while (Out.EndsWith(TEXT("_")))
		{
			Out.LeftChopInline(1);
		}
		return Out;
	}

	static bool IsSupportedType(const FString& Type)
	{
		static const TSet<FString> Supported = {
			TEXT("static_mesh"),
			TEXT("instanced_static_mesh"),
			TEXT("hierarchical_instanced_static_mesh"),
			TEXT("dynamic_mesh"),
			TEXT("skeletal_mesh"),
			TEXT("voxel_collision"),
			TEXT("primitive_collision"),
			TEXT("spline_mesh"),
			TEXT("geometry_cache"),
		};
		return Supported.Contains(Type);
	}

	static bool ParseFilters(const TSharedPtr<FJsonObject>& Params, FFilters& OutFilters, FString& OutError)
	{
		TSharedPtr<FJsonObject> FiltersObj = Params;
		const TSharedPtr<FJsonObject>* NestedFilters = nullptr;
		if (Params.IsValid() && Params->TryGetObjectField(TEXT("filters"), NestedFilters) && NestedFilters && NestedFilters->IsValid())
		{
			FiltersObj = *NestedFilters;
		}

		TryGetStringField(FiltersObj, TEXT("folder_path_prefix"), OutFilters.FolderPathPrefix);
		if (OutFilters.FolderPathPrefix.TrimStartAndEnd().IsEmpty())
		{
			TryGetStringField(FiltersObj, TEXT("accept_folder_path_prefix"), OutFilters.FolderPathPrefix);
		}
		TryGetStringField(FiltersObj, TEXT("ignore_folder_path_prefix"), OutFilters.IgnoreFolderPathPrefix);
		OutFilters.FolderPathPrefix = UeAgentLevelOps::NormalizeFolderPathForMatch(OutFilters.FolderPathPrefix);
		OutFilters.IgnoreFolderPathPrefix = UeAgentLevelOps::NormalizeFolderPathForMatch(OutFilters.IgnoreFolderPathPrefix);
		ParseStringArray(FiltersObj, TEXT("accept_tags"), OutFilters.AcceptTags);
		ParseStringArray(FiltersObj, TEXT("ignore_tags"), OutFilters.IgnoreTags);

		TArray<FString> TypeStrings;
		ParseStringArray(FiltersObj, TEXT("types"), TypeStrings);
		for (const FString& TypeString : TypeStrings)
		{
			const FString Type = NormalizeTypeToken(TypeString);
			if (Type.IsEmpty())
			{
				continue;
			}
			if (!IsSupportedType(Type))
			{
				OutError = FString::Printf(TEXT("unsupported_type:%s"), *Type);
				return false;
			}
			OutFilters.Types.Add(Type);
		}
		return true;
	}

	static bool ActorPassesFilters(AActor* Actor, const FFilters& Filters)
	{
		if (!Actor || Actor->IsActorBeingDestroyed())
		{
			return false;
		}
		if (!Filters.FolderPathPrefix.IsEmpty()
			&& !UeAgentLevelOps::DoesActorMatchFolderPath(Actor, Filters.FolderPathPrefix, true))
		{
			return false;
		}
		if (!Filters.IgnoreFolderPathPrefix.IsEmpty()
			&& UeAgentLevelOps::DoesActorMatchFolderPath(Actor, Filters.IgnoreFolderPathPrefix, true))
		{
			return false;
		}
		for (const FString& Tag : Filters.AcceptTags)
		{
			if (!Actor->ActorHasTag(*Tag))
			{
				return false;
			}
		}
		for (const FString& Tag : Filters.IgnoreTags)
		{
			if (Actor->ActorHasTag(*Tag))
			{
				return false;
			}
		}
		return true;
	}

	static bool ParseAabbQuery(const TSharedPtr<FJsonObject>& QueryObj, FQuery& OutQuery, FString& OutError)
	{
		FVector Min = FVector::ZeroVector;
		FVector Max = FVector::ZeroVector;
		if (TryGetVectorField(QueryObj, TEXT("min"), Min) && TryGetVectorField(QueryObj, TEXT("max"), Max))
		{
			OutQuery.Type = EQueryShapeType::Aabb;
			OutQuery.Aabb = FBox(Min.ComponentMin(Max), Min.ComponentMax(Max));
			return OutQuery.Aabb.IsValid != 0;
		}

		FVector Center = FVector::ZeroVector;
		FVector Extent = FVector::ZeroVector;
		if (TryGetVectorField(QueryObj, TEXT("center"), Center) && TryGetVectorField(QueryObj, TEXT("extent"), Extent))
		{
			Extent = Extent.GetAbs();
			OutQuery.Type = EQueryShapeType::Aabb;
			OutQuery.Aabb = FBox(Center - Extent, Center + Extent);
			return OutQuery.Aabb.IsValid != 0;
		}

		OutError = TEXT("missing_aabb_min_max_or_center_extent");
		return false;
	}

	static bool ParseSphereQuery(const TSharedPtr<FJsonObject>& QueryObj, FQuery& OutQuery, FString& OutError)
	{
		FVector Center = FVector::ZeroVector;
		double Radius = 0.0;
		if (!TryGetVectorField(QueryObj, TEXT("center"), Center) || !TryGetNumberField(QueryObj, TEXT("radius"), Radius) || Radius <= 0.0)
		{
			OutError = TEXT("missing_sphere_center_or_radius");
			return false;
		}
		OutQuery.Type = EQueryShapeType::Sphere;
		OutQuery.Center = Center;
		OutQuery.Radius = Radius;
		return true;
	}

	static bool ParseObbQuery(const TSharedPtr<FJsonObject>& QueryObj, FQuery& OutQuery, FString& OutError)
	{
		FVector Center = FVector::ZeroVector;
		FVector AxisX = FVector::ForwardVector;
		FVector AxisY = FVector::RightVector;
		FVector AxisZ = FVector::UpVector;
		FVector Half = FVector::ZeroVector;
		if (!TryGetVectorField(QueryObj, TEXT("center"), Center)
			|| !TryGetVectorField(QueryObj, TEXT("axis_x"), AxisX)
			|| !TryGetVectorField(QueryObj, TEXT("axis_y"), AxisY)
			|| !TryGetVectorField(QueryObj, TEXT("axis_z"), AxisZ))
		{
			OutError = TEXT("missing_obb_center_or_axes");
			return false;
		}
		if (!TryGetVectorField(QueryObj, TEXT("half_extent"), Half)
			&& !TryGetVectorField(QueryObj, TEXT("half_extents"), Half)
			&& !TryGetVectorField(QueryObj, TEXT("extent"), Half))
		{
			OutError = TEXT("missing_obb_half_extent");
			return false;
		}
		AxisX.Normalize();
		AxisY.Normalize();
		AxisZ.Normalize();
		Half = Half.GetAbs();
		if (Half.X <= 0.0 || Half.Y <= 0.0 || Half.Z <= 0.0)
		{
			OutError = TEXT("invalid_obb_half_extent");
			return false;
		}

		OutQuery.Type = EQueryShapeType::Obb;
		OutQuery.Obb.Center = Center;
		OutQuery.Obb.Axis[0] = AxisX;
		OutQuery.Obb.Axis[1] = AxisY;
		OutQuery.Obb.Axis[2] = AxisZ;
		OutQuery.Obb.Half = Half;
		return true;
	}

	static bool ParseQuery(const TSharedPtr<FJsonObject>& Params, FQuery& OutQuery, FString& OutError)
	{
		const TSharedPtr<FJsonObject>* QueryObjPtr = nullptr;
		TSharedPtr<FJsonObject> QueryObj = Params;
		if (Params.IsValid() && Params->TryGetObjectField(TEXT("query"), QueryObjPtr) && QueryObjPtr && QueryObjPtr->IsValid())
		{
			QueryObj = *QueryObjPtr;
		}

		FString Type = TEXT("sphere");
		TryGetStringField(QueryObj, TEXT("type"), Type);
		Type = NormalizeTypeToken(Type);
		if (Type == TEXT("box"))
		{
			Type = TEXT("aabb");
		}

		if (Type == TEXT("aabb"))
		{
			return ParseAabbQuery(QueryObj, OutQuery, OutError);
		}
		if (Type == TEXT("sphere"))
		{
			return ParseSphereQuery(QueryObj, OutQuery, OutError);
		}
		if (Type == TEXT("obb"))
		{
			return ParseObbQuery(QueryObj, OutQuery, OutError);
		}

		OutError = FString::Printf(TEXT("unsupported_query_type:%s"), *Type);
		return false;
	}

	static double AxisValue(const FVector& Value, int32 AxisIndex)
	{
		return AxisIndex == 0 ? Value.X : (AxisIndex == 1 ? Value.Y : Value.Z);
	}

	static FObb MakeAabbObb(const FBox& Box)
	{
		FObb Obb;
		Obb.Center = Box.GetCenter();
		Obb.Axis[0] = FVector::ForwardVector;
		Obb.Axis[1] = FVector::RightVector;
		Obb.Axis[2] = FVector::UpVector;
		Obb.Half = Box.GetExtent();
		return Obb;
	}

	static FObb MakeWorldObbFromLocalAabb(const FTransform& OwnerToWorld, const FVector& LocalCenter, const FVector& LocalExtent)
	{
		FObb Obb;
		Obb.Center = OwnerToWorld.TransformPosition(LocalCenter);
		const FVector UnitAxes[3] = { FVector::ForwardVector, FVector::RightVector, FVector::UpVector };
		for (int32 AxisIndex = 0; AxisIndex < 3; ++AxisIndex)
		{
			const FVector ScaledAxis = OwnerToWorld.TransformVector(UnitAxes[AxisIndex]);
			const double AxisScale = ScaledAxis.Size();
			Obb.Axis[AxisIndex] = AxisScale > KINDA_SMALL_NUMBER ? ScaledAxis / AxisScale : UnitAxes[AxisIndex];
			if (AxisIndex == 0)
			{
				Obb.Half.X = LocalExtent.X * AxisScale;
			}
			else if (AxisIndex == 1)
			{
				Obb.Half.Y = LocalExtent.Y * AxisScale;
			}
			else
			{
				Obb.Half.Z = LocalExtent.Z * AxisScale;
			}
		}
		return Obb;
	}

	static bool OverlapObbSphere(const FObb& Obb, const FVector& SphereCenter, double Radius)
	{
		FVector Closest = Obb.Center;
		const FVector Delta = SphereCenter - Obb.Center;
		for (int32 AxisIndex = 0; AxisIndex < 3; ++AxisIndex)
		{
			const double Projected = FVector::DotProduct(Delta, Obb.Axis[AxisIndex]);
			const double Clamped = FMath::Clamp(Projected, -AxisValue(Obb.Half, AxisIndex), AxisValue(Obb.Half, AxisIndex));
			Closest += Obb.Axis[AxisIndex] * Clamped;
		}
		return FVector::DistSquared(Closest, SphereCenter) <= Radius * Radius;
	}

	static bool OverlapObbObb(const FObb& A, const FObb& B)
	{
		const double Epsilon = 1e-6;
		double R[3][3];
		double AbsR[3][3];
		for (int32 I = 0; I < 3; ++I)
		{
			for (int32 J = 0; J < 3; ++J)
			{
				R[I][J] = FVector::DotProduct(A.Axis[I], B.Axis[J]);
				AbsR[I][J] = FMath::Abs(R[I][J]) + Epsilon;
			}
		}

		const FVector Delta = B.Center - A.Center;
		double T[3] = {
			FVector::DotProduct(Delta, A.Axis[0]),
			FVector::DotProduct(Delta, A.Axis[1]),
			FVector::DotProduct(Delta, A.Axis[2]),
		};

		for (int32 I = 0; I < 3; ++I)
		{
			const double Ra = AxisValue(A.Half, I);
			const double Rb =
				AxisValue(B.Half, 0) * AbsR[I][0] +
				AxisValue(B.Half, 1) * AbsR[I][1] +
				AxisValue(B.Half, 2) * AbsR[I][2];
			if (FMath::Abs(T[I]) > Ra + Rb)
			{
				return false;
			}
		}

		for (int32 J = 0; J < 3; ++J)
		{
			const double Ra =
				AxisValue(A.Half, 0) * AbsR[0][J] +
				AxisValue(A.Half, 1) * AbsR[1][J] +
				AxisValue(A.Half, 2) * AbsR[2][J];
			const double Rb = AxisValue(B.Half, J);
			const double Projected = FMath::Abs(FVector::DotProduct(Delta, B.Axis[J]));
			if (Projected > Ra + Rb)
			{
				return false;
			}
		}

		for (int32 I = 0; I < 3; ++I)
		{
			for (int32 J = 0; J < 3; ++J)
			{
				const int32 I1 = (I + 1) % 3;
				const int32 I2 = (I + 2) % 3;
				const int32 J1 = (J + 1) % 3;
				const int32 J2 = (J + 2) % 3;
				const double Ra = AxisValue(A.Half, I1) * AbsR[I2][J] + AxisValue(A.Half, I2) * AbsR[I1][J];
				const double Rb = AxisValue(B.Half, J1) * AbsR[I][J2] + AxisValue(B.Half, J2) * AbsR[I][J1];
				const double Projected = FMath::Abs(T[I2] * R[I1][J] - T[I1] * R[I2][J]);
				if (Projected > Ra + Rb)
				{
					return false;
				}
			}
		}

		return true;
	}

	static bool OverlapQuery(const FQuery& Query, const FObb& ShapeObb)
	{
		switch (Query.Type)
		{
		case EQueryShapeType::Sphere:
			return OverlapObbSphere(ShapeObb, Query.Center, Query.Radius);
		case EQueryShapeType::Obb:
			return OverlapObbObb(ShapeObb, Query.Obb);
		default:
			return OverlapObbObb(ShapeObb, MakeAabbObb(Query.Aabb));
		}
	}

	static FBox AabbFromTransformedLocalBox(const FTransform& LocalTransform, const FVector& Extent)
	{
		FBox Box(EForceInit::ForceInit);
		for (int32 SignX = -1; SignX <= 1; SignX += 2)
		{
			for (int32 SignY = -1; SignY <= 1; SignY += 2)
			{
				for (int32 SignZ = -1; SignZ <= 1; SignZ += 2)
				{
					const FVector LocalCorner(Extent.X * SignX, Extent.Y * SignY, Extent.Z * SignZ);
					Box += LocalTransform.TransformPosition(LocalCorner);
				}
			}
		}
		return Box;
	}

	static bool TryAppendShape(
		const FString& Src,
		const FBox& LocalBox,
		const FTransform& OwnerToWorld,
		const FQuery& Query,
		const FOutputOptions& OutputOptions,
		int32 MaxShapesPerActor,
		int32& InOutReturnedShapeCount,
		int32& InOutOmittedShapeCount,
		TArray<TSharedPtr<FJsonValue>>& OutShapes)
	{
		if (!LocalBox.IsValid)
		{
			return false;
		}

		const FVector LocalCenter = LocalBox.GetCenter();
		const FVector LocalExtent = LocalBox.GetExtent();
		const FObb WorldObb = MakeWorldObbFromLocalAabb(OwnerToWorld, LocalCenter, LocalExtent);
		if (!OverlapQuery(Query, WorldObb))
		{
			return false;
		}

		if (MaxShapesPerActor > 0 && InOutReturnedShapeCount >= MaxShapesPerActor)
		{
			++InOutOmittedShapeCount;
			return false;
		}

		TSharedPtr<FJsonObject> ShapeObj = MakeShared<FJsonObject>();
		ShapeObj->SetStringField(TEXT("src"), Src);
		ShapeObj->SetField(TEXT("center"), Vec3Value(LocalCenter));
		ShapeObj->SetField(TEXT("extent"), Vec3Value(LocalExtent));
		if (OutputOptions.bStandard || OutputOptions.bDebug)
		{
			FString Source;
			FString Authority;
			SplitSourceAuthority(Src, Source, Authority);
			ShapeObj->SetStringField(TEXT("source"), Source);
			ShapeObj->SetStringField(TEXT("authority"), Authority);
			ShapeObj->SetObjectField(TEXT("world_bounds"), CompactBoundsJson(TransformLocalAabbToWorldAabb(LocalBox, OwnerToWorld)));
		}
		if (OutputOptions.bDebug)
		{
			ShapeObj->SetObjectField(TEXT("owner_to_world"), CompactTransformJson(OwnerToWorld));
		}
		OutShapes.Add(MakeShared<FJsonValueObject>(ShapeObj));
		++InOutReturnedShapeCount;
		return true;
	}

	static bool IsQueryShapeEnabled(const FKShapeElem& Shape)
	{
		return CollisionEnabledHasQuery(Shape.GetCollisionEnabled());
	}

	static int32 BodySetupSimpleShapeCount(const UBodySetup* BodySetup)
	{
		if (!BodySetup)
		{
			return 0;
		}

		int32 Count = 0;
		for (const FKBoxElem& Elem : BodySetup->AggGeom.BoxElems)
		{
			Count += IsQueryShapeEnabled(Elem) ? 1 : 0;
		}
		for (const FKSphereElem& Elem : BodySetup->AggGeom.SphereElems)
		{
			Count += IsQueryShapeEnabled(Elem) ? 1 : 0;
		}
		for (const FKSphylElem& Elem : BodySetup->AggGeom.SphylElems)
		{
			Count += IsQueryShapeEnabled(Elem) ? 1 : 0;
		}
		for (const FKConvexElem& Elem : BodySetup->AggGeom.ConvexElems)
		{
			Count += IsQueryShapeEnabled(Elem) ? 1 : 0;
		}
		return Count;
	}

	static bool AppendBodySetupShapes(
		const UBodySetup* BodySetup,
		const FTransform& OwnerToWorld,
		const FQuery& Query,
		const FOutputOptions& OutputOptions,
		int32 MaxShapesPerActor,
		int32& InOutReturnedShapeCount,
		int32& InOutOmittedShapeCount,
		TArray<TSharedPtr<FJsonValue>>& OutShapes)
	{
		if (!BodySetup || BodySetupSimpleShapeCount(BodySetup) <= 0)
		{
			return false;
		}

		const int32 ShapeCountBefore = OutShapes.Num();
		for (const FKBoxElem& Elem : BodySetup->AggGeom.BoxElems)
		{
			if (!IsQueryShapeEnabled(Elem))
			{
				continue;
			}
			const FTransform LocalTransform(Elem.Rotation, Elem.Center, FVector::OneVector);
			const FVector Extent(Elem.X * 0.5, Elem.Y * 0.5, Elem.Z * 0.5);
			TryAppendShape(TEXT("collision_box/block"), AabbFromTransformedLocalBox(LocalTransform, Extent), OwnerToWorld, Query, OutputOptions, MaxShapesPerActor, InOutReturnedShapeCount, InOutOmittedShapeCount, OutShapes);
		}
		for (const FKSphereElem& Elem : BodySetup->AggGeom.SphereElems)
		{
			if (!IsQueryShapeEnabled(Elem))
			{
				continue;
			}
			const FVector Extent(Elem.Radius, Elem.Radius, Elem.Radius);
			TryAppendShape(TEXT("collision_sphere/block"), FBox(Elem.Center - Extent, Elem.Center + Extent), OwnerToWorld, Query, OutputOptions, MaxShapesPerActor, InOutReturnedShapeCount, InOutOmittedShapeCount, OutShapes);
		}
		for (const FKSphylElem& Elem : BodySetup->AggGeom.SphylElems)
		{
			if (!IsQueryShapeEnabled(Elem))
			{
				continue;
			}
			const FTransform LocalTransform(Elem.Rotation, Elem.Center, FVector::OneVector);
			const FVector Extent(Elem.Radius, Elem.Radius, Elem.Length * 0.5 + Elem.Radius);
			TryAppendShape(TEXT("collision_capsule/block"), AabbFromTransformedLocalBox(LocalTransform, Extent), OwnerToWorld, Query, OutputOptions, MaxShapesPerActor, InOutReturnedShapeCount, InOutOmittedShapeCount, OutShapes);
		}
		for (const FKConvexElem& Elem : BodySetup->AggGeom.ConvexElems)
		{
			if (!IsQueryShapeEnabled(Elem))
			{
				continue;
			}
			FBox ConvexBox(EForceInit::ForceInit);
			const FTransform LocalTransform = Elem.GetTransform();
			for (const FVector& Vertex : Elem.VertexData)
			{
				ConvexBox += LocalTransform.TransformPosition(Vertex);
			}
			TryAppendShape(TEXT("collision_convex/block"), ConvexBox, OwnerToWorld, Query, OutputOptions, MaxShapesPerActor, InOutReturnedShapeCount, InOutOmittedShapeCount, OutShapes);
		}
		return OutShapes.Num() > ShapeCountBefore;
	}

	static bool AppendVisualFallback(
		const FBox& LocalBox,
		const FTransform& OwnerToWorld,
		const FQuery& Query,
		const FOutputOptions& OutputOptions,
		int32 MaxShapesPerActor,
		int32& InOutReturnedShapeCount,
		int32& InOutOmittedShapeCount,
		TArray<TSharedPtr<FJsonValue>>& OutShapes)
	{
		return TryAppendShape(TEXT("visual_aabb/visual"), LocalBox, OwnerToWorld, Query, OutputOptions, MaxShapesPerActor, InOutReturnedShapeCount, InOutOmittedShapeCount, OutShapes);
	}

	static FBox ComponentLocalBounds(UPrimitiveComponent* Component)
	{
		if (!Component)
		{
			return FBox(EForceInit::ForceInit);
		}
		const FBoxSphereBounds LocalBounds = Component->CalcBounds(FTransform::Identity);
		return FBox(LocalBounds.Origin - LocalBounds.BoxExtent, LocalBounds.Origin + LocalBounds.BoxExtent);
	}

	static FBox StaticMeshLocalBounds(UStaticMesh* StaticMesh)
	{
		if (!StaticMesh)
		{
			return FBox(EForceInit::ForceInit);
		}
		const FBoxSphereBounds MeshBounds = StaticMesh->GetBounds();
		return FBox(MeshBounds.Origin - MeshBounds.BoxExtent, MeshBounds.Origin + MeshBounds.BoxExtent);
	}

	static FBox ComponentWorldBounds(UPrimitiveComponent* Component)
	{
		if (!Component)
		{
			return FBox(EForceInit::ForceInit);
		}
		FBoxSphereBounds Bounds = Component->Bounds;
		if (Bounds.BoxExtent.IsNearlyZero())
		{
			Bounds = Component->CalcBounds(Component->GetComponentTransform());
		}
		return FBox(Bounds.Origin - Bounds.BoxExtent, Bounds.Origin + Bounds.BoxExtent);
	}

	static bool IsVoxelCollisionComponent(const UPrimitiveComponent* Component)
	{
		if (!Component || !Component->IsA<UBoxComponent>())
		{
			return false;
		}
		if (Component->GetName().StartsWith(TEXT("UAI_VoxelCollision_")))
		{
			return true;
		}
		for (const FName& Tag : Component->ComponentTags)
		{
			if (Tag.ToString().StartsWith(VoxelCollisionTagPrefix))
			{
				return true;
			}
		}
		return false;
	}

	static FString ComponentQueryType(UPrimitiveComponent* Component)
	{
		if (!Component)
		{
			return TEXT("");
		}
		if (IsVoxelCollisionComponent(Component))
		{
			return TEXT("voxel_collision");
		}
		if (Component->IsA<UHierarchicalInstancedStaticMeshComponent>())
		{
			return TEXT("hierarchical_instanced_static_mesh");
		}
		if (Component->IsA<UInstancedStaticMeshComponent>())
		{
			return TEXT("instanced_static_mesh");
		}
		if (Component->IsA<USplineMeshComponent>())
		{
			return TEXT("spline_mesh");
		}
		if (Component->IsA<UStaticMeshComponent>())
		{
			return TEXT("static_mesh");
		}
		if (Component->IsA<UDynamicMeshComponent>())
		{
			return TEXT("dynamic_mesh");
		}
		if (Component->IsA<USkeletalMeshComponent>())
		{
			return TEXT("skeletal_mesh");
		}
		if (Component->IsA<UGeometryCacheComponent>())
		{
			return TEXT("geometry_cache");
		}
		if (Component->IsA<UBoxComponent>() || Component->IsA<USphereComponent>() || Component->IsA<UCapsuleComponent>())
		{
			return TEXT("primitive_collision");
		}
		return TEXT("");
	}

	static bool FilterAllowsComponentType(const FFilters& Filters, const FString& Type)
	{
		if (Filters.Types.Num() == 0 || Filters.Types.Contains(Type))
		{
			return true;
		}
		return Type == TEXT("voxel_collision") && Filters.Types.Contains(TEXT("primitive_collision"));
	}

	static bool AppendPrimitiveComponentShape(
		UPrimitiveComponent* Component,
		const FQuery& Query,
		const FOutputOptions& OutputOptions,
		int32 MaxShapesPerActor,
		int32& InOutReturnedShapeCount,
		int32& InOutOmittedShapeCount,
		TArray<TSharedPtr<FJsonValue>>& OutShapes)
	{
		if (!Component || Component->GetCollisionEnabled() == ECollisionEnabled::NoCollision)
		{
			return false;
		}

		const FTransform OwnerToWorld = Component->GetComponentTransform();
		if (const UBoxComponent* BoxComponent = Cast<UBoxComponent>(Component))
		{
			const FVector Extent = BoxComponent->GetUnscaledBoxExtent();
			return TryAppendShape(TEXT("collision_box/block"), FBox(-Extent, Extent), OwnerToWorld, Query, OutputOptions, MaxShapesPerActor, InOutReturnedShapeCount, InOutOmittedShapeCount, OutShapes);
		}
		if (const USphereComponent* SphereComponent = Cast<USphereComponent>(Component))
		{
			const double Radius = SphereComponent->GetUnscaledSphereRadius();
			const FVector Extent(Radius, Radius, Radius);
			return TryAppendShape(TEXT("collision_sphere/block"), FBox(-Extent, Extent), OwnerToWorld, Query, OutputOptions, MaxShapesPerActor, InOutReturnedShapeCount, InOutOmittedShapeCount, OutShapes);
		}
		if (const UCapsuleComponent* CapsuleComponent = Cast<UCapsuleComponent>(Component))
		{
			const double Radius = CapsuleComponent->GetUnscaledCapsuleRadius();
			const double HalfHeight = CapsuleComponent->GetUnscaledCapsuleHalfHeight();
			const FVector Extent(Radius, Radius, HalfHeight);
			return TryAppendShape(TEXT("collision_capsule/block"), FBox(-Extent, Extent), OwnerToWorld, Query, OutputOptions, MaxShapesPerActor, InOutReturnedShapeCount, InOutOmittedShapeCount, OutShapes);
		}
		return false;
	}

	static TSharedPtr<FJsonObject> MakeOwnerJson(UPrimitiveComponent* Component, const FTransform& OwnerToWorld, const FString& ComponentType = FString(), const FOutputOptions* OutputOptions = nullptr, int32 InstanceIndex = INDEX_NONE)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Component ? Component->GetName() : TEXT(""));
		Obj->SetStringField(TEXT("class"), (Component && Component->GetClass()) ? Component->GetClass()->GetName() : FString());
		if (InstanceIndex != INDEX_NONE)
		{
			Obj->SetNumberField(TEXT("instance_index"), InstanceIndex);
		}
		Obj->SetObjectField(TEXT("to_world"), CompactTransformJson(OwnerToWorld));
		if (OutputOptions && (OutputOptions->bStandard || OutputOptions->bDebug) && !ComponentType.IsEmpty())
		{
			Obj->SetStringField(TEXT("type"), ComponentType);
			Obj->SetStringField(TEXT("path"), Component ? Component->GetPathName() : TEXT(""));
		}
		return Obj;
	}

	static bool BuildMeshComponentOwner(
		UStaticMeshComponent* Component,
		const FTransform& OwnerToWorld,
		const FString& ComponentType,
		int32 InstanceIndex,
		const FQuery& Query,
		const FOutputOptions& OutputOptions,
		bool bIncludeVisualFallback,
		int32 MaxShapesPerActor,
		int32& InOutReturnedShapeCount,
		int32& InOutOmittedShapeCount,
		TSharedPtr<FJsonObject>& OutOwner)
	{
		OutOwner.Reset();
		if (!Component || !Component->GetStaticMesh())
		{
			return false;
		}

		const int32 OmittedBefore = InOutOmittedShapeCount;
		TArray<TSharedPtr<FJsonValue>> Shapes;
		const bool bCollisionEnabled = Component->GetCollisionEnabled() != ECollisionEnabled::NoCollision;
		const UBodySetup* BodySetup = Component->GetStaticMesh()->GetBodySetup();
		if (bCollisionEnabled && BodySetupSimpleShapeCount(BodySetup) > 0)
		{
			AppendBodySetupShapes(BodySetup, OwnerToWorld, Query, OutputOptions, MaxShapesPerActor, InOutReturnedShapeCount, InOutOmittedShapeCount, Shapes);
		}
		else if (bIncludeVisualFallback)
		{
			AppendVisualFallback(StaticMeshLocalBounds(Component->GetStaticMesh()), OwnerToWorld, Query, OutputOptions, MaxShapesPerActor, InOutReturnedShapeCount, InOutOmittedShapeCount, Shapes);
		}

		if (Shapes.Num() <= 0)
		{
			return false;
		}

		OutOwner = MakeOwnerJson(Component, OwnerToWorld, ComponentType, &OutputOptions, InstanceIndex);
		OutOwner->SetArrayField(TEXT("shapes"), Shapes);
		if (InOutOmittedShapeCount > OmittedBefore)
		{
			OutOwner->SetNumberField(TEXT("omitted_shapes"), InOutOmittedShapeCount - OmittedBefore);
		}
		return true;
	}

	static bool BuildFallbackOwner(
		UPrimitiveComponent* Component,
		const FQuery& Query,
		const FOutputOptions& OutputOptions,
		bool bIncludeVisualFallback,
		int32 MaxShapesPerActor,
		int32& InOutReturnedShapeCount,
		int32& InOutOmittedShapeCount,
		TSharedPtr<FJsonObject>& OutOwner)
	{
		OutOwner.Reset();
		if (!Component || !bIncludeVisualFallback)
		{
			return false;
		}

		const int32 OmittedBefore = InOutOmittedShapeCount;
		TArray<TSharedPtr<FJsonValue>> Shapes;
		AppendVisualFallback(ComponentLocalBounds(Component), Component->GetComponentTransform(), Query, OutputOptions, MaxShapesPerActor, InOutReturnedShapeCount, InOutOmittedShapeCount, Shapes);
		if (Shapes.Num() <= 0)
		{
			return false;
		}

		OutOwner = MakeOwnerJson(Component, Component->GetComponentTransform(), ComponentQueryType(Component), &OutputOptions);
		OutOwner->SetArrayField(TEXT("shapes"), Shapes);
		if (InOutOmittedShapeCount > OmittedBefore)
		{
			OutOwner->SetNumberField(TEXT("omitted_shapes"), InOutOmittedShapeCount - OmittedBefore);
		}
		return true;
	}

	static TSharedPtr<FJsonObject> MakeActorJson(AActor* Actor, int32 ActorIndex)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("id"), FString::Printf(TEXT("A%d"), ActorIndex));
		Obj->SetStringField(TEXT("name"), Actor ? Actor->GetActorLabel() : TEXT(""));
		Obj->SetStringField(TEXT("class"), (Actor && Actor->GetClass()) ? Actor->GetClass()->GetName() : TEXT(""));
		Obj->SetStringField(TEXT("folder"), Actor ? Actor->GetFolderPath().ToString() : TEXT(""));
		if (Actor)
		{
			Obj->SetObjectField(TEXT("t"), CompactTransformJson(Actor->GetActorTransform()));
			FBox Bounds(EForceInit::ForceInit);
			FString BoundsError;
			if (UeAgentLevelOps::GetActorBoundsBox(Actor, Bounds, BoundsError))
			{
				Obj->SetObjectField(TEXT("b"), CompactBoundsJson(Bounds));
			}
		}
		return Obj;
	}

	static TSharedPtr<FJsonObject> QueryJson(const FQuery& Query)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("frame"), TEXT("world"));
		switch (Query.Type)
		{
		case EQueryShapeType::Sphere:
			Obj->SetStringField(TEXT("type"), TEXT("sphere"));
			Obj->SetField(TEXT("center"), Vec3Value(Query.Center));
			Obj->SetNumberField(TEXT("radius"), Query.Radius);
			break;
		case EQueryShapeType::Obb:
			Obj->SetStringField(TEXT("type"), TEXT("obb"));
			Obj->SetField(TEXT("center"), Vec3Value(Query.Obb.Center));
			Obj->SetField(TEXT("axis_x"), Vec3Value(Query.Obb.Axis[0]));
			Obj->SetField(TEXT("axis_y"), Vec3Value(Query.Obb.Axis[1]));
			Obj->SetField(TEXT("axis_z"), Vec3Value(Query.Obb.Axis[2]));
			Obj->SetField(TEXT("half_extent"), Vec3Value(Query.Obb.Half));
			break;
		default:
			Obj->SetStringField(TEXT("type"), TEXT("aabb"));
			Obj->SetField(TEXT("min"), Vec3Value(Query.Aabb.Min));
			Obj->SetField(TEXT("max"), Vec3Value(Query.Aabb.Max));
			break;
		}
		return Obj;
	}
}

bool FUeAgentLegacyMechanics::CmdLevelGetNearbyMeshShapes(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentLevelMeshShapes;

	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	FQuery Query;
	if (!ParseQuery(Ctx.Params, Query, OutError))
	{
		return false;
	}

	FFilters Filters;
	if (!ParseFilters(Ctx.Params, Filters, OutError))
	{
		return false;
	}

	FString DetailLevel = TEXT("summary");
	TryGetStringField(Ctx.Params, TEXT("detail_level"), DetailLevel);
	DetailLevel = DetailLevel.TrimStartAndEnd().ToLower();
	if (DetailLevel.IsEmpty())
	{
		DetailLevel = TEXT("summary");
	}
	if (DetailLevel != TEXT("summary") && DetailLevel != TEXT("standard") && DetailLevel != TEXT("debug"))
	{
		OutError = FString::Printf(TEXT("unsupported_detail_level:%s"), *DetailLevel);
		return false;
	}

	FString OutputStyle = TEXT("compact");
	TryGetStringField(Ctx.Params, TEXT("output_style"), OutputStyle);
	OutputStyle = OutputStyle.TrimStartAndEnd().ToLower();
	if (OutputStyle.IsEmpty())
	{
		OutputStyle = TEXT("compact");
	}
	if (OutputStyle != TEXT("compact"))
	{
		OutError = FString::Printf(TEXT("unsupported_output_style:%s"), *OutputStyle);
		return false;
	}

	FOutputOptions OutputOptions;
	OutputOptions.bStandard = DetailLevel == TEXT("standard") || DetailLevel == TEXT("debug");
	OutputOptions.bDebug = DetailLevel == TEXT("debug");

	bool bIncludeVisualFallback = true;
	TryGetBoolField(Ctx.Params, TEXT("include_visual_fallback"), bIncludeVisualFallback);

	double MaxShapesNumber = 64.0;
	TryGetNumberField(Ctx.Params, TEXT("max_shapes_per_actor"), MaxShapesNumber);
	const int32 MaxShapesPerActor = FMath::Clamp(FMath::RoundToInt(MaxShapesNumber), 1, 10000);

	double LimitNumber = 1000.0;
	TryGetNumberField(Ctx.Params, TEXT("limit"), LimitNumber);
	const int32 ActorLimit = FMath::Clamp(FMath::RoundToInt(LimitNumber), 0, 10000);

	TArray<TSharedPtr<FJsonValue>> ActorValues;
	int32 ActorIndex = 0;
	int32 MatchedComponentCount = 0;
	int32 OmittedActorCount = 0;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!ActorPassesFilters(Actor, Filters))
		{
			continue;
		}

		TArray<TSharedPtr<FJsonValue>> ComponentValues;
		int32 ReturnedShapeCount = 0;
		int32 OmittedShapeCount = 0;
		int32 ActorComponentOwnerCount = 0;

		TInlineComponentArray<UPrimitiveComponent*> PrimitiveComponents(Actor);
		for (UPrimitiveComponent* Component : PrimitiveComponents)
		{
			if (!Component || !Component->IsRegistered())
			{
				continue;
			}

			const FString ComponentType = ComponentQueryType(Component);
			if (ComponentType.IsEmpty() || !FilterAllowsComponentType(Filters, ComponentType))
			{
				continue;
			}

			const FBox WorldBounds = ComponentWorldBounds(Component);
			if (!WorldBounds.IsValid || !OverlapQuery(Query, MakeAabbObb(WorldBounds)))
			{
				continue;
			}

			if (UInstancedStaticMeshComponent* InstancedComponent = Cast<UInstancedStaticMeshComponent>(Component))
			{
				for (int32 InstanceIndex = 0; InstanceIndex < InstancedComponent->GetInstanceCount(); ++InstanceIndex)
				{
					FTransform InstanceToWorld;
					if (!InstancedComponent->GetInstanceTransform(InstanceIndex, InstanceToWorld, true))
					{
						continue;
					}

					TSharedPtr<FJsonObject> Owner;
					if (BuildMeshComponentOwner(InstancedComponent, InstanceToWorld, ComponentType, InstanceIndex, Query, OutputOptions, bIncludeVisualFallback, MaxShapesPerActor, ReturnedShapeCount, OmittedShapeCount, Owner))
					{
						ComponentValues.Add(MakeShared<FJsonValueObject>(Owner));
						++ActorComponentOwnerCount;
					}
				}
				continue;
			}

			TSharedPtr<FJsonObject> Owner;
			if (UStaticMeshComponent* StaticMeshComponent = Cast<UStaticMeshComponent>(Component))
			{
				if (ComponentType == TEXT("spline_mesh"))
				{
					if (BuildFallbackOwner(Component, Query, OutputOptions, bIncludeVisualFallback, MaxShapesPerActor, ReturnedShapeCount, OmittedShapeCount, Owner))
					{
						ComponentValues.Add(MakeShared<FJsonValueObject>(Owner));
						++ActorComponentOwnerCount;
					}
				}
				else if (BuildMeshComponentOwner(StaticMeshComponent, StaticMeshComponent->GetComponentTransform(), ComponentType, INDEX_NONE, Query, OutputOptions, bIncludeVisualFallback, MaxShapesPerActor, ReturnedShapeCount, OmittedShapeCount, Owner))
				{
					ComponentValues.Add(MakeShared<FJsonValueObject>(Owner));
					++ActorComponentOwnerCount;
				}
				continue;
			}

			if (ComponentType == TEXT("primitive_collision") || ComponentType == TEXT("voxel_collision"))
			{
				TArray<TSharedPtr<FJsonValue>> Shapes;
				if (AppendPrimitiveComponentShape(Component, Query, OutputOptions, MaxShapesPerActor, ReturnedShapeCount, OmittedShapeCount, Shapes) && Shapes.Num() > 0)
				{
					Owner = MakeOwnerJson(Component, Component->GetComponentTransform(), ComponentType, &OutputOptions);
					Owner->SetArrayField(TEXT("shapes"), Shapes);
					ComponentValues.Add(MakeShared<FJsonValueObject>(Owner));
					++ActorComponentOwnerCount;
				}
				continue;
			}

			if (BuildFallbackOwner(Component, Query, OutputOptions, bIncludeVisualFallback, MaxShapesPerActor, ReturnedShapeCount, OmittedShapeCount, Owner))
			{
				ComponentValues.Add(MakeShared<FJsonValueObject>(Owner));
				++ActorComponentOwnerCount;
			}
		}

		if (ComponentValues.Num() <= 0)
		{
			continue;
		}

		if (ActorLimit > 0 && ActorValues.Num() >= ActorLimit)
		{
			++OmittedActorCount;
			continue;
		}

		++ActorIndex;
		TSharedPtr<FJsonObject> ActorObj = MakeActorJson(Actor, ActorIndex);
		ActorObj->SetArrayField(TEXT("components"), ComponentValues);
		if (OmittedShapeCount > 0)
		{
			ActorObj->SetNumberField(TEXT("omitted_shapes"), OmittedShapeCount);
		}
		ActorValues.Add(MakeShared<FJsonValueObject>(ActorObj));
		MatchedComponentCount += ActorComponentOwnerCount;
	}

	OutData->SetObjectField(TEXT("q"), QueryJson(Query));
	OutData->SetStringField(TEXT("detail_level"), DetailLevel);
	OutData->SetStringField(TEXT("output_style"), OutputStyle);
	OutData->SetNumberField(TEXT("actor_count"), ActorValues.Num());
	OutData->SetNumberField(TEXT("component_owner_count"), MatchedComponentCount);
	OutData->SetObjectField(TEXT("shape_space"), ShapeSpaceJson());
	if (OmittedActorCount > 0)
	{
		OutData->SetNumberField(TEXT("omitted_actors"), OmittedActorCount);
	}
	OutData->SetArrayField(TEXT("actors"), ActorValues);
	return true;
}

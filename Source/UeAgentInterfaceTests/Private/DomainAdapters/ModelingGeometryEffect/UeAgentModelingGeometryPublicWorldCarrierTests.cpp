#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Composition/SimpleAuthoring/OperationHost/UeAgentSimpleAuthoringOperationHost.h"
#include "Core/StateSemantic/UeAgentStateRevisionJson.h"
#include "DomainAdapters/ModelingGeometryEffect/UeAgentModelingGeometryEffectCarrierRevision.h"
#include "DomainAdapters/ModelingGeometryEffect/UeAgentModelingGeometryEffectNormalObservation.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMeshActor.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "Generators/SphereGenerator.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "PackageTools.h"
#include "PhysicsEngine/AggregateGeom.h"
#include "PhysicsEngine/BodySetup.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "TestSupport/UeAgentPublicCommandTestHarness.h"
#include "UDynamicMesh.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"

#include <initializer_list>

namespace UE::UAI::Tests::ModelingGeometryPublicWorldCarrier
{
	using namespace UE::Geometry;
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::CommandRegistry;
	using namespace UE::UAI::DomainAdapters::ModelingGeometryEffect;
	using namespace UE::UAI::ExecutionMutation;
	using namespace UE::UAI::StateSemantic;

	class FOwnedPublicWorld final
	{
	public:
		FOwnedPublicWorld()
		{
			const FString Suffix = FGuid::NewGuid().ToString(
				EGuidFormats::Digits);
			PackageName =
				TEXT("/Game/UAI/Tests/ModelingGeometry/WorldCarrier_")
				+ Suffix;
			HeaderPath = FPackageName::LongPackageNameToFilename(
				PackageName,
				FPackageName::GetMapPackageExtension());
			Package = CreatePackage(*PackageName);
			World = Package
				? UWorld::CreateWorld(
					EWorldType::EditorPreview,
					false,
					FName(*FPackageName::GetLongPackageAssetName(PackageName)),
					Package)
				: nullptr;
			if (!World || !World->PersistentLevel || !GEngine)
			{
				return;
			}
			World->SetFlags(RF_Public | RF_Standalone);
			World->PersistentLevel->SetUseExternalActors(false);
			World->SetCurrentLevel(World->PersistentLevel);
			FWorldContext& Context = GEngine->CreateNewWorldContext(
				EWorldType::EditorPreview);
			Context.SetCurrentWorld(World);
			bHasWorldContext = true;
			Package->SetDirtyFlag(false);
		}

		~FOwnedPublicWorld()
		{
			ReleaseLoadedWorld();
			for (const FString& Path : {
				HeaderPath,
				FPaths::ChangeExtension(HeaderPath, TEXT("uexp")),
				FPaths::ChangeExtension(HeaderPath, TEXT("ubulk")),
				FPaths::ChangeExtension(HeaderPath, TEXT("upayload"))})
			{
				IFileManager::Get().Delete(*Path, false, true, true);
			}
		}

		bool IsValid() const
		{
			return Package
				&& World
				&& World->PersistentLevel
				&& bHasWorldContext;
		}

		bool TryReload(FString& OutError)
		{
			OutError.Reset();
			if (!IsValid())
			{
				OutError = TEXT("modeling_world_reload_source_invalid");
				return false;
			}
			const TWeakObjectPtr<UWorld> OriginalWorld(World);
			ReleaseLoadedWorld();
			if (OriginalWorld.IsValid())
			{
				OutError = TEXT("modeling_world_reload_source_still_live");
				return false;
			}
			Package = LoadPackage(nullptr, *PackageName, LOAD_None);
			World = Package ? UWorld::FindWorldInPackage(Package) : nullptr;
			if (!World || !World->PersistentLevel)
			{
				OutError = TEXT("modeling_world_reload_failed:") + PackageName;
				return false;
			}
			World->SetCurrentLevel(World->PersistentLevel);
			return true;
		}

		FString PackageName;
		FString HeaderPath;
		UPackage* Package = nullptr;
		UWorld* World = nullptr;

	private:
		void ReleaseLoadedWorld()
		{
			UPackage* const PackageToUnload = Package;
			UWorld* const WorldToDestroy = World;
			if (bHasWorldContext && GEngine && WorldToDestroy)
			{
				GEngine->DestroyWorldContext(WorldToDestroy);
				bHasWorldContext = false;
			}
			if (PackageToUnload)
			{
				PackageToUnload->SetDirtyFlag(false);
			}
			if (WorldToDestroy)
			{
				WorldToDestroy->DestroyWorld(false);
				WorldToDestroy->MarkAsGarbage();
			}
			World = nullptr;
			Package = nullptr;
			if (PackageToUnload)
			{
				FText UnloadError;
				UPackageTools::UnloadPackages(
					{PackageToUnload},
					UnloadError,
					true);
			}
			CollectGarbage(RF_NoFlags, true);
		}

		bool bHasWorldContext = false;
	};

	void AppendCube(
		FDynamicMesh3& Mesh,
		const FVector3d& Center,
		const double HalfExtent,
		const int32 GroupBase)
	{
		const int32 V0 = Mesh.AppendVertex(
			Center + FVector3d(-HalfExtent, -HalfExtent, -HalfExtent));
		const int32 V1 = Mesh.AppendVertex(
			Center + FVector3d(HalfExtent, -HalfExtent, -HalfExtent));
		const int32 V2 = Mesh.AppendVertex(
			Center + FVector3d(HalfExtent, HalfExtent, -HalfExtent));
		const int32 V3 = Mesh.AppendVertex(
			Center + FVector3d(-HalfExtent, HalfExtent, -HalfExtent));
		const int32 V4 = Mesh.AppendVertex(
			Center + FVector3d(-HalfExtent, -HalfExtent, HalfExtent));
		const int32 V5 = Mesh.AppendVertex(
			Center + FVector3d(HalfExtent, -HalfExtent, HalfExtent));
		const int32 V6 = Mesh.AppendVertex(
			Center + FVector3d(HalfExtent, HalfExtent, HalfExtent));
		const int32 V7 = Mesh.AppendVertex(
			Center + FVector3d(-HalfExtent, HalfExtent, HalfExtent));
		const FIndex3i Triangles[] = {
			{V0, V2, V1}, {V0, V3, V2},
			{V4, V5, V6}, {V4, V6, V7},
			{V0, V1, V5}, {V0, V5, V4},
			{V1, V2, V6}, {V1, V6, V5},
			{V2, V3, V7}, {V2, V7, V6},
			{V3, V0, V4}, {V3, V4, V7},
		};
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(Triangles); ++Index)
		{
			Mesh.AppendTriangle(
				Triangles[Index],
				GroupBase + Index / 2);
		}
	}

	void BuildCube(FDynamicMesh3& Mesh)
	{
		Mesh.Clear();
		Mesh.EnableTriangleGroups();
		AppendCube(Mesh, FVector3d::Zero(), 50.0, 0);
		Mesh.ReverseOrientation(false);
		Mesh.EnableAttributes();
	}

	void BuildTwoDisconnectedCubes(FDynamicMesh3& Mesh)
	{
		Mesh.Clear();
		Mesh.EnableTriangleGroups();
		AppendCube(Mesh, FVector3d(-75.0, 0.0, 0.0), 25.0, 0);
		AppendCube(Mesh, FVector3d(75.0, 0.0, 0.0), 25.0, 6);
		Mesh.ReverseOrientation(false);
		Mesh.EnableAttributes();
	}

	void BuildOctahedron(
		FDynamicMesh3& Mesh,
		const double Radius,
		const double HalfHeight)
	{
		Mesh.Clear();
		Mesh.EnableTriangleGroups();
		const int32 XPositive =
			Mesh.AppendVertex(FVector3d(Radius, 0.0, 0.0));
		const int32 XNegative =
			Mesh.AppendVertex(FVector3d(-Radius, 0.0, 0.0));
		const int32 YPositive =
			Mesh.AppendVertex(FVector3d(0.0, Radius, 0.0));
		const int32 YNegative =
			Mesh.AppendVertex(FVector3d(0.0, -Radius, 0.0));
		const int32 ZPositive =
			Mesh.AppendVertex(FVector3d(0.0, 0.0, HalfHeight));
		const int32 ZNegative =
			Mesh.AppendVertex(FVector3d(0.0, 0.0, -HalfHeight));
		const FIndex3i Triangles[] = {
			{ZPositive, XPositive, YPositive},
			{ZPositive, YPositive, XNegative},
			{ZPositive, XNegative, YNegative},
			{ZPositive, YNegative, XPositive},
			{ZNegative, YPositive, XPositive},
			{ZNegative, XNegative, YPositive},
			{ZNegative, YNegative, XNegative},
			{ZNegative, XPositive, YNegative},
		};
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(Triangles); ++Index)
		{
			Mesh.AppendTriangle(Triangles[Index], Index);
		}
		Mesh.EnableAttributes();
	}

	void BuildDenseSphere(FDynamicMesh3& Mesh)
	{
		FSphereGenerator Generator;
		Generator.Radius = 50.0;
		Generator.NumPhi = 16;
		Generator.NumTheta = 16;
		Generator.bPolygroupPerQuad = false;
		Generator.Generate();
		Mesh = FDynamicMesh3(&Generator);
	}

	void BuildOpenGrid(FDynamicMesh3& Mesh)
	{
		constexpr int32 SideVertexCount = 5;
		constexpr double Spacing = 25.0;
		Mesh.Clear();
		Mesh.EnableTriangleGroups();
		int32 Vertices[SideVertexCount][SideVertexCount];
		for (int32 Y = 0; Y < SideVertexCount; ++Y)
		{
			for (int32 X = 0; X < SideVertexCount; ++X)
			{
				Vertices[Y][X] = Mesh.AppendVertex(FVector3d(
					(X - 2) * Spacing,
					(Y - 2) * Spacing,
					0.0));
			}
		}
		for (int32 Y = 0; Y < SideVertexCount - 1; ++Y)
		{
			for (int32 X = 0; X < SideVertexCount - 1; ++X)
			{
				const int32 V00 = Vertices[Y][X];
				const int32 V10 = Vertices[Y][X + 1];
				const int32 V01 = Vertices[Y + 1][X];
				const int32 V11 = Vertices[Y + 1][X + 1];
				Mesh.AppendTriangle(V00, V10, V11, 0);
				Mesh.AppendTriangle(V00, V11, V01, 0);
			}
		}
		Mesh.EnableAttributes();
	}

	ADynamicMeshActor* FindActorByLabel(
		UWorld& World,
		const FString& Label)
	{
		ADynamicMeshActor* Match = nullptr;
		for (TActorIterator<ADynamicMeshActor> It(&World); It; ++It)
		{
			if (It->GetActorLabel() != Label)
			{
				continue;
			}
			if (Match)
			{
				return nullptr;
			}
			Match = *It;
		}
		return Match;
	}

	ADynamicMeshActor* FindActorByGuid(
		UWorld& World,
		const FGuid& ActorGuid)
	{
		ADynamicMeshActor* Match = nullptr;
		for (TActorIterator<ADynamicMeshActor> It(&World); It; ++It)
		{
			if (It->GetActorGuid() != ActorGuid)
			{
				continue;
			}
			if (Match)
			{
				return nullptr;
			}
			Match = *It;
		}
		return Match;
	}

	struct FMeshObservation final
	{
		FVector3d BoundsMin = FVector3d::Zero();
		FVector3d BoundsMax = FVector3d::Zero();
		int32 TriangleCount = 0;
		int32 BoundaryEdgeCount = 0;
		TArray<FVector3d> BoundaryPositions;
		int32 CollisionShapeCount = 0;
		int32 CollisionBoxCount = 0;
		int32 CollisionSphereCount = 0;
		int32 CollisionCapsuleCount = 0;
		int32 CollisionConvexCount = 0;
		bool bCollisionBoundsValid = false;
		FVector3d CollisionBoundsMin = FVector3d::Zero();
		FVector3d CollisionBoundsMax = FVector3d::Zero();
		FNormalSemanticObservation NormalObservation;
	};

	bool ReadMeshObservation(
		ADynamicMeshActor& Actor,
		FMeshObservation& OutObservation)
	{
		OutObservation = FMeshObservation{};
		UDynamicMeshComponent* const Component =
			Actor.GetDynamicMeshComponent();
		UDynamicMesh* const DynamicMesh =
			Component ? Component->GetDynamicMesh() : nullptr;
		if (!DynamicMesh)
		{
			return false;
		}
		bool bRead = false;
		DynamicMesh->ProcessMesh(
			[&](const FDynamicMesh3& Mesh)
			{
				bool bHasVertex = false;
				for (const int32 VertexId : Mesh.VertexIndicesItr())
				{
					const FVector3d Position = Mesh.GetVertex(VertexId);
					if (!bHasVertex)
					{
						OutObservation.BoundsMin = Position;
						OutObservation.BoundsMax = Position;
						bHasVertex = true;
						continue;
					}
					OutObservation.BoundsMin.X = FMath::Min(
						OutObservation.BoundsMin.X,
						Position.X);
					OutObservation.BoundsMin.Y = FMath::Min(
						OutObservation.BoundsMin.Y,
						Position.Y);
					OutObservation.BoundsMin.Z = FMath::Min(
						OutObservation.BoundsMin.Z,
						Position.Z);
					OutObservation.BoundsMax.X = FMath::Max(
						OutObservation.BoundsMax.X,
						Position.X);
					OutObservation.BoundsMax.Y = FMath::Max(
						OutObservation.BoundsMax.Y,
						Position.Y);
					OutObservation.BoundsMax.Z = FMath::Max(
						OutObservation.BoundsMax.Z,
						Position.Z);
				}
				OutObservation.TriangleCount = Mesh.TriangleCount();
				OutObservation.NormalObservation =
					ObserveNormalSemantics(Mesh);
				for (const int32 EdgeId : Mesh.EdgeIndicesItr())
				{
					if (Mesh.IsBoundaryEdge(EdgeId))
					{
						++OutObservation.BoundaryEdgeCount;
					}
				}
				for (const int32 VertexId : Mesh.VertexIndicesItr())
				{
					if (Mesh.IsBoundaryVertex(VertexId))
					{
						OutObservation.BoundaryPositions.Add(
							Mesh.GetVertex(VertexId));
					}
				}
				OutObservation.BoundaryPositions.Sort(
					[](const FVector3d& Left, const FVector3d& Right)
					{
						if (Left.X != Right.X)
						{
							return Left.X < Right.X;
						}
						if (Left.Y != Right.Y)
						{
							return Left.Y < Right.Y;
						}
						return Left.Z < Right.Z;
					});
				bRead = bHasVertex && OutObservation.TriangleCount > 0;
			});
		if (const UBodySetup* const BodySetup = Component->GetBodySetup())
		{
			const FKAggregateGeom& Collision = BodySetup->AggGeom;
			OutObservation.CollisionBoxCount = Collision.BoxElems.Num();
			OutObservation.CollisionSphereCount = Collision.SphereElems.Num();
			OutObservation.CollisionCapsuleCount =
				Collision.SphylElems.Num()
				+ Collision.TaperedCapsuleElems.Num();
			OutObservation.CollisionConvexCount =
				Collision.ConvexElems.Num();
			OutObservation.CollisionShapeCount =
				Collision.GetElementCount();
			if (OutObservation.CollisionShapeCount > 0)
			{
				const FBox CollisionBounds =
					Collision.CalcAABB(FTransform::Identity);
				OutObservation.bCollisionBoundsValid =
					CollisionBounds.IsValid != 0;
				if (OutObservation.bCollisionBoundsValid)
				{
					OutObservation.CollisionBoundsMin =
						FVector3d(CollisionBounds.Min);
					OutObservation.CollisionBoundsMax =
						FVector3d(CollisionBounds.Max);
				}
			}
		}
		return bRead;
	}

	bool IsNearlyEqualVector(
		const FVector3d& Actual,
		const FVector3d& Expected,
		const double Tolerance = 0.25)
	{
		return FMath::IsNearlyEqual(Actual.X, Expected.X, Tolerance)
			&& FMath::IsNearlyEqual(Actual.Y, Expected.Y, Tolerance)
			&& FMath::IsNearlyEqual(Actual.Z, Expected.Z, Tolerance);
	}

	bool HaveSamePositions(
		const TArray<FVector3d>& Actual,
		const TArray<FVector3d>& Expected,
		const double Tolerance = 0.001)
	{
		if (Actual.Num() != Expected.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < Actual.Num(); ++Index)
		{
			if (!IsNearlyEqualVector(
					Actual[Index],
					Expected[Index],
					Tolerance))
			{
				return false;
			}
		}
		return true;
	}

	bool HaveSameGeometry(
		const FMeshObservation& Actual,
		const FMeshObservation& Expected)
	{
		return Actual.TriangleCount == Expected.TriangleCount
			&& Actual.BoundaryEdgeCount == Expected.BoundaryEdgeCount
			&& HaveSamePositions(
				Actual.BoundaryPositions,
				Expected.BoundaryPositions)
			&& IsNearlyEqualVector(Actual.BoundsMin, Expected.BoundsMin)
			&& IsNearlyEqualVector(Actual.BoundsMax, Expected.BoundsMax);
	}

	bool CollisionBoundsMatchMesh(const FMeshObservation& Observation)
	{
		return Observation.bCollisionBoundsValid
			&& IsNearlyEqualVector(
				Observation.CollisionBoundsMin,
				Observation.BoundsMin,
				1.0)
			&& IsNearlyEqualVector(
				Observation.CollisionBoundsMax,
				Observation.BoundsMax,
				1.0);
	}

	bool SeedSphereCollision(ADynamicMeshActor& Actor)
	{
		UDynamicMeshComponent* const Component =
			Actor.GetDynamicMeshComponent();
		if (!Component)
		{
			return false;
		}
		FKAggregateGeom Collision;
		FKSphereElem Sphere;
		Sphere.Center = FVector::ZeroVector;
		Sphere.Radius = 50.0;
		Collision.SphereElems.Add(Sphere);
		Component->SetSimpleCollisionShapes(Collision, false);
		UBodySetup* const BodySetup = Component->GetBodySetup();
		if (!BodySetup)
		{
			return false;
		}
		BodySetup->AggGeom = Collision;
		return true;
	}

	void SeedMixedHardEdgeNormals(UDynamicMesh& DynamicMesh)
	{
		DynamicMesh.EditMesh(
			[](FDynamicMesh3& Mesh)
			{
				if (!Mesh.HasAttributes())
				{
					Mesh.EnableAttributes();
				}
				FDynamicMeshNormalOverlay* const Normals =
					Mesh.Attributes()->PrimaryNormals();
				const double DotThreshold = FMath::Cos(
					FMath::DegreesToRadians(30.0));
				Normals->CreateFromPredicate(
					[&Mesh, DotThreshold](
						int32,
						const int32 FirstTriangleId,
						const int32 SecondTriangleId)
					{
						return FVector3d::DotProduct(
							Mesh.GetTriNormal(FirstTriangleId),
							Mesh.GetTriNormal(SecondTriangleId))
							> DotThreshold;
					},
					0.0f);
				int32 ElementOrdinal = 0;
				for (const int32 ElementId : Normals->ElementIndicesItr())
				{
					const float DirectionAngle =
						0.37f * static_cast<float>(++ElementOrdinal);
					Normals->SetElement(
						ElementId,
						FVector3f(
							0.6f * FMath::Cos(DirectionAngle),
							0.6f * FMath::Sin(DirectionAngle),
							0.8f));
				}
			});
	}

	bool HasCompleteMixedHardEdgeSeed(
		const FNormalSemanticObservation& Observation)
	{
		return Observation.InteriorEdgeCount > 0
			&& Observation.InvalidInteriorEdgeCount == 0
			&& Observation.PartiallySplitInteriorEdgeCount == 0
			&& Observation.FullySmoothInteriorEdgeCount > 0
			&& Observation.FullySplitInteriorEdgeCount > 0
			&& Observation.SplitInteriorEdgeEndpointKeys.Num() > 0
			&& Observation.SplitInteriorEdgeEndpointKeys.Num()
				< Observation.InteriorEdgeCount * 2
			&& Observation.ReferencedNormalElementCount > 0
			&& Observation.WeightedMisalignedNormalElementCount
				== Observation.ReferencedNormalElementCount;
	}

	bool HasCompletePreservedHardEdgeObservation(
		const FNormalSemanticObservation& Before,
		const FNormalSemanticObservation& After)
	{
		return HasCompleteMixedHardEdgeSeed(Before)
			&& IsCompleteWeightedNormalObservation(After)
			&& After.InteriorEdgeCount == Before.InteriorEdgeCount
			&& After.InvalidInteriorEdgeCount == 0
			&& After.PartiallySplitInteriorEdgeCount == 0
			&& After.FullySmoothInteriorEdgeCount
				== Before.FullySmoothInteriorEdgeCount
			&& After.FullySplitInteriorEdgeCount
				== Before.FullySplitInteriorEdgeCount
			&& After.SplitInteriorEdgeEndpointKeys
				== Before.SplitInteriorEdgeEndpointKeys;
	}

	FString DescribeCollisionObservation(
		const FMeshObservation& Observation)
	{
		return FString::Printf(
			TEXT("shapes=%d boxes=%d spheres=%d capsules=%d convex=%d "
				"mesh_min=(%.6f,%.6f,%.6f) mesh_max=(%.6f,%.6f,%.6f) "
				"collision_valid=%s collision_min=(%.6f,%.6f,%.6f) "
				"collision_max=(%.6f,%.6f,%.6f)"),
			Observation.CollisionShapeCount,
			Observation.CollisionBoxCount,
			Observation.CollisionSphereCount,
			Observation.CollisionCapsuleCount,
			Observation.CollisionConvexCount,
			Observation.BoundsMin.X,
			Observation.BoundsMin.Y,
			Observation.BoundsMin.Z,
			Observation.BoundsMax.X,
			Observation.BoundsMax.Y,
			Observation.BoundsMax.Z,
			Observation.bCollisionBoundsValid ? TEXT("true") : TEXT("false"),
			Observation.CollisionBoundsMin.X,
			Observation.CollisionBoundsMin.Y,
			Observation.CollisionBoundsMin.Z,
			Observation.CollisionBoundsMax.X,
			Observation.CollisionBoundsMax.Y,
			Observation.CollisionBoundsMax.Z);
	}

	TArray<TSharedPtr<FJsonValue>> NumberValues(
		const std::initializer_list<double> Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		for (const double Value : Values)
		{
			Result.Add(MakeShared<FJsonValueNumber>(Value));
		}
		return Result;
	}

	FQuat ExpectedPrimitiveRotation()
	{
		FQuat Rotation(
			0.0,
			0.0,
			0.7071067811865476,
			0.7071067811865476);
		Rotation.Normalize();
		return Rotation;
	}

	FVector3d ExpectedPrimitiveScale()
	{
		return FVector3d(1.25, 0.75, 1.5);
	}

	TSharedRef<FJsonObject> BuildPlacement(const FVector3d& Translation)
	{
		const TSharedRef<FJsonObject> Placement = MakeShared<FJsonObject>();
		Placement->SetStringField(TEXT("semantic_frame_id"), TEXT("ue.world"));
		Placement->SetStringField(TEXT("write_frame_id"), TEXT("ue.world"));
		Placement->SetStringField(TEXT("read_frame_id"), TEXT("ue.world"));
		Placement->SetStringField(TEXT("verify_frame_id"), TEXT("ue.world"));
		Placement->SetStringField(TEXT("unit"), TEXT("centimeter"));
		Placement->SetArrayField(
			TEXT("translation"),
			NumberValues({Translation.X, Translation.Y, Translation.Z}));
		Placement->SetArrayField(
			TEXT("rotation_quaternion_xyzw"),
			NumberValues({
				ExpectedPrimitiveRotation().X,
				ExpectedPrimitiveRotation().Y,
				ExpectedPrimitiveRotation().Z,
				ExpectedPrimitiveRotation().W}));
		Placement->SetArrayField(
			TEXT("scale"),
			NumberValues({
				ExpectedPrimitiveScale().X,
				ExpectedPrimitiveScale().Y,
				ExpectedPrimitiveScale().Z}));
		Placement->SetStringField(
			TEXT("basis_evidence"),
			TEXT("explicit normalized UE world placement"));
		Placement->SetStringField(
			TEXT("conversion_chain"),
			TEXT(
				"semantic world -> normalized UE world -> actor transform -> "
				"independent persisted-world readback"));
		return Placement;
	}

	struct FPrimitiveWorldCase final
	{
		FString Key;
		FString CommandId;
		FString BeautifiedName;
		FVector3d ExpectedBoundsSize = FVector3d::Zero();
		FVector3d ExpectedLocation = FVector3d::Zero();
	};

	const TArray<FPrimitiveWorldCase>& GetPrimitiveWorldCases()
	{
		static const TArray<FPrimitiveWorldCase> Cases = {
			{TEXT("box"), TEXT("mesh.primitive.box.create"),
				TEXT("Representative.Box"), FVector3d(120.0, 80.0, 40.0),
				FVector3d(100.0, 200.0, 300.0)},
			{TEXT("ramp_corner"), TEXT("mesh.primitive.ramp_corner.create"),
				TEXT("Representative.RampCorner"), FVector3d(120.0, 80.0, 40.0),
				FVector3d(220.0, 200.0, 300.0)},
			{TEXT("cylinder"), TEXT("mesh.primitive.cylinder.create"),
				TEXT("Expansion.Cylinder"), FVector3d(70.0, 70.0, 90.0),
				FVector3d(340.0, 200.0, 300.0)},
			{TEXT("plane"), TEXT("mesh.primitive.plane.create"),
				TEXT("Expansion.Plane"), FVector3d(120.0, 80.0, 0.0),
				FVector3d(460.0, 200.0, 300.0)},
			{TEXT("ramp"), TEXT("mesh.primitive.ramp.create"),
				TEXT("Expansion.Ramp"), FVector3d(120.0, 80.0, 40.0),
				FVector3d(580.0, 200.0, 300.0)},
			{TEXT("sphere"), TEXT("mesh.primitive.sphere.create"),
				TEXT("Expansion.Sphere"), FVector3d(100.0, 100.0, 100.0),
				FVector3d(700.0, 200.0, 300.0)},
			{TEXT("stairs"), TEXT("mesh.primitive.stairs.create"),
				TEXT("Expansion.Stairs"), FVector3d(120.0, 100.0, 80.0),
				FVector3d(820.0, 200.0, 300.0)},
		};
		return Cases;
	}

	const FPrimitiveWorldCase* FindPrimitiveWorldCase(const FString& Key)
	{
		return GetPrimitiveWorldCases().FindByPredicate(
			[&](const FPrimitiveWorldCase& Candidate)
			{
				return Candidate.Key == Key;
			});
	}

	TSharedRef<FJsonObject> BuildPrimitiveParams(
		const FPrimitiveWorldCase& TestCase,
		const FString& TargetRef)
	{
		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target_ref"), TargetRef);
		Params->SetObjectField(
			TEXT("placement"),
			BuildPlacement(TestCase.ExpectedLocation));
		if (TestCase.Key == TEXT("box"))
		{
			Params->SetArrayField(
				TEXT("size_cm"),
				NumberValues({120.0, 80.0, 40.0}));
			Params->SetArrayField(
				TEXT("subdivisions"),
				NumberValues({2.0, 3.0, 4.0}));
			Params->SetStringField(TEXT("anchor"), TEXT("base_center"));
		}
		else if (TestCase.Key == TEXT("cylinder"))
		{
			Params->SetNumberField(TEXT("radius_cm"), 35.0);
			Params->SetNumberField(TEXT("height_cm"), 90.0);
			Params->SetNumberField(TEXT("radial_segments"), 12.0);
			Params->SetNumberField(TEXT("height_segments"), 3.0);
			Params->SetBoolField(TEXT("cap_ends"), true);
			Params->SetStringField(TEXT("anchor"), TEXT("base_center"));
		}
		else if (TestCase.Key == TEXT("plane"))
		{
			Params->SetArrayField(
				TEXT("size_cm"),
				NumberValues({120.0, 80.0}));
			Params->SetArrayField(
				TEXT("subdivisions"),
				NumberValues({3.0, 2.0}));
			Params->SetBoolField(TEXT("double_sided"), true);
		}
		else if (TestCase.Key == TEXT("ramp"))
		{
			Params->SetArrayField(
				TEXT("size_cm"),
				NumberValues({120.0, 80.0, 40.0}));
			Params->SetStringField(TEXT("slope_axis"), TEXT("x"));
			Params->SetStringField(TEXT("rise_direction"), TEXT("negative"));
		}
		else if (TestCase.Key == TEXT("ramp_corner"))
		{
			Params->SetArrayField(
				TEXT("size_cm"),
				NumberValues({120.0, 80.0, 40.0}));
			Params->SetStringField(
				TEXT("high_corner"),
				TEXT("negative_x_negative_y"));
		}
		else if (TestCase.Key == TEXT("sphere"))
		{
			Params->SetNumberField(TEXT("radius_cm"), 50.0);
			Params->SetNumberField(TEXT("longitude_segments"), 12.0);
			Params->SetNumberField(TEXT("latitude_segments"), 8.0);
		}
		else if (TestCase.Key == TEXT("stairs"))
		{
			Params->SetNumberField(TEXT("step_count"), 4.0);
			Params->SetNumberField(TEXT("step_width_cm"), 100.0);
			Params->SetNumberField(TEXT("step_depth_cm"), 30.0);
			Params->SetNumberField(TEXT("step_height_cm"), 20.0);
			Params->SetStringField(TEXT("rise_axis"), TEXT("x"));
			Params->SetStringField(TEXT("rise_direction"), TEXT("negative"));
		}
		return Params;
	}

	bool IsVerifiedWorldArtifactOutcome(const FString& PayloadJson)
	{
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(PayloadJson);
		const TSharedPtr<FJsonObject>* Projection = nullptr;
		const TSharedPtr<FJsonObject>* Payload = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* EvidenceIds = nullptr;
		FString Status;
		FString EffectOutcome;
		if (!FJsonSerializer::Deserialize(Reader, Root)
			|| !Root.IsValid()
			|| !Root->TryGetObjectField(TEXT("projection"), Projection)
			|| !Projection || !Projection->IsValid()
			|| !(*Projection)->TryGetObjectField(TEXT("payload"), Payload)
			|| !Payload || !Payload->IsValid()
			|| !(*Payload)->TryGetStringField(TEXT("status"), Status)
			|| !(*Payload)->TryGetStringField(
				TEXT("effect_outcome"),
				EffectOutcome)
			|| !(*Payload)->TryGetArrayField(TEXT("evidence_ids"), EvidenceIds)
			|| !EvidenceIds)
		{
			return false;
		}
		return Status == TEXT("verified")
			&& EffectOutcome == TEXT("complete")
			&& EvidenceIds->ContainsByPredicate(
				[](const TSharedPtr<FJsonValue>& Value)
				{
					return Value.IsValid()
						&& Value->Type == EJson::String
						&& Value->AsString().StartsWith(
							TEXT("modeling:persisted-world-instance:"));
					});
	}

	FString ReadCommandErrorCode(const FUeAgentCommandResult& Result)
	{
		TSharedPtr<FJsonObject> Error;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Result.GetErrorJson());
		FString Code;
		return FJsonSerializer::Deserialize(Reader, Error)
			&& Error.IsValid()
			&& Error->TryGetStringField(TEXT("code"), Code)
				? Code
				: FString();
	}

	enum class EExistingTargetWorldSemantic : uint8
	{
		SubdivideUniformLevelOne,
		SimplifyTriangleRatioHalf,
		SimplifyToleranceOpenGrid,
		SimplifyOpenBoundaryPreserved,
		SimplifyOpenBoundaryFree,
		SimplifyOpenBoundaryUnreachable,
		SimpleCollisionAlignedBox,
		SimpleCollisionMinVolume,
		SimpleCollisionCapsule,
		SimpleCollisionSphere,
		SimpleCollisionAlignedBoxAppend,
		ConvexCollisionSingleHull,
		ConvexCollisionDecomposition,
		NormalsFace,
		NormalsAreaAngle,
		NormalsAngleThreshold,
		NormalsAreaAnglePreserve,
	};

	enum class EExistingTargetWorldSeed : uint8
	{
		ClosedCube,
		OpenGrid,
		SphereLike,
		DenseSphereLike,
		CapsuleLike,
		DisconnectedCubes,
	};

	struct FExistingTargetWorldCase final
	{
		FString Scope;
		FString ActorName;
		FString CommandId;
		FString RequestId;
		EExistingTargetWorldSemantic Semantic =
			EExistingTargetWorldSemantic::SubdivideUniformLevelOne;
		EExistingTargetWorldSeed Seed =
			EExistingTargetWorldSeed::ClosedCube;
		FString ExpectedErrorCode;
	};

	TSharedRef<FJsonObject> BuildExistingTargetWorldParams(
		const FExistingTargetWorldCase& TestCase,
		const FString& TargetRef,
		const FUeAgentStateRevision& Revision)
	{
		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target_ref"), TargetRef);
		Params->SetObjectField(
			TEXT("target_revision"),
			FUeAgentStateRevisionJson::ToJson(Revision));
		switch (TestCase.Semantic)
		{
		case EExistingTargetWorldSemantic::SubdivideUniformLevelOne:
			Params->SetStringField(TEXT("scheme"), TEXT("uniform"));
			Params->SetNumberField(TEXT("levels"), 1);
			break;
		case EExistingTargetWorldSemantic::SimplifyTriangleRatioHalf:
			Params->SetStringField(
				TEXT("target_mode"),
				TEXT("triangle_ratio"));
			Params->SetNumberField(TEXT("target_value"), 0.5);
			Params->SetBoolField(TEXT("preserve_boundaries"), false);
			break;
		case EExistingTargetWorldSemantic::SimplifyToleranceOpenGrid:
			Params->SetStringField(
				TEXT("target_mode"),
				TEXT("tolerance_cm"));
			Params->SetNumberField(TEXT("target_value"), 0.001);
			Params->SetBoolField(TEXT("preserve_boundaries"), false);
			break;
		case EExistingTargetWorldSemantic::SimplifyOpenBoundaryPreserved:
		case EExistingTargetWorldSemantic::SimplifyOpenBoundaryFree:
		case EExistingTargetWorldSemantic::SimplifyOpenBoundaryUnreachable:
			Params->SetStringField(
				TEXT("target_mode"),
				TEXT("triangle_count"));
			Params->SetNumberField(
				TEXT("target_value"),
				TestCase.Semantic ==
					EExistingTargetWorldSemantic::
						SimplifyOpenBoundaryPreserved
						? 24.0
						: 4.0);
			Params->SetBoolField(
				TEXT("preserve_boundaries"),
				TestCase.Semantic !=
					EExistingTargetWorldSemantic::
						SimplifyOpenBoundaryFree);
			break;
		case EExistingTargetWorldSemantic::SimpleCollisionAlignedBox:
			Params->SetStringField(TEXT("method"), TEXT("aligned_boxes"));
			Params->SetNumberField(TEXT("max_shapes"), 1);
			Params->SetBoolField(TEXT("replace_existing"), true);
			break;
		case EExistingTargetWorldSemantic::SimpleCollisionMinVolume:
			Params->SetStringField(TEXT("method"), TEXT("min_volume_shapes"));
			Params->SetNumberField(TEXT("max_shapes"), 1);
			Params->SetBoolField(TEXT("replace_existing"), true);
			break;
		case EExistingTargetWorldSemantic::SimpleCollisionCapsule:
			Params->SetStringField(TEXT("method"), TEXT("capsules"));
			Params->SetNumberField(TEXT("max_shapes"), 1);
			Params->SetBoolField(TEXT("replace_existing"), true);
			break;
		case EExistingTargetWorldSemantic::SimpleCollisionSphere:
			Params->SetStringField(TEXT("method"), TEXT("spheres"));
			Params->SetNumberField(TEXT("max_shapes"), 1);
			Params->SetBoolField(TEXT("replace_existing"), true);
			break;
		case EExistingTargetWorldSemantic::SimpleCollisionAlignedBoxAppend:
			Params->SetStringField(TEXT("method"), TEXT("aligned_boxes"));
			Params->SetNumberField(TEXT("max_shapes"), 1);
			Params->SetBoolField(TEXT("replace_existing"), false);
			break;
		case EExistingTargetWorldSemantic::ConvexCollisionSingleHull:
		case EExistingTargetWorldSemantic::ConvexCollisionDecomposition:
			Params->SetStringField(
				TEXT("mode"),
				TestCase.Semantic ==
					EExistingTargetWorldSemantic::ConvexCollisionSingleHull
						? TEXT("single_hull")
						: TEXT("decomposition"));
			Params->SetNumberField(
				TEXT("max_hulls"),
				TestCase.Semantic ==
					EExistingTargetWorldSemantic::ConvexCollisionSingleHull
						? 1
						: 2);
			Params->SetNumberField(TEXT("max_vertices_per_hull"), 64);
			Params->SetBoolField(TEXT("replace_existing"), true);
			break;
		case EExistingTargetWorldSemantic::NormalsFace:
			Params->SetStringField(TEXT("mode"), TEXT("face"));
			Params->SetBoolField(
				TEXT("preserve_existing_hard_edges"),
				false);
			break;
		case EExistingTargetWorldSemantic::NormalsAreaAngle:
		case EExistingTargetWorldSemantic::NormalsAreaAnglePreserve:
			Params->SetStringField(
				TEXT("mode"),
				TEXT("area_angle_weighted"));
			Params->SetBoolField(
				TEXT("preserve_existing_hard_edges"),
				TestCase.Semantic ==
					EExistingTargetWorldSemantic::
						NormalsAreaAnglePreserve);
			break;
		case EExistingTargetWorldSemantic::NormalsAngleThreshold:
			Params->SetStringField(
				TEXT("mode"),
				TEXT("angle_threshold"));
			Params->SetNumberField(TEXT("angle_degrees"), 30.0);
			Params->SetBoolField(
				TEXT("preserve_existing_hard_edges"),
				false);
			break;
		}
		return Params;
	}

	bool MatchesExistingTargetWorldSemantic(
		const FExistingTargetWorldCase& TestCase,
		const FMeshObservation& Before,
		const FMeshObservation& After)
	{
		if (After.TriangleCount <= 0)
		{
			return false;
		}
		switch (TestCase.Semantic)
		{
		case EExistingTargetWorldSemantic::SubdivideUniformLevelOne:
			return Before.TriangleCount == 12
				&& After.TriangleCount == 48;
		case EExistingTargetWorldSemantic::SimplifyTriangleRatioHalf:
			return Before.TriangleCount == 12
				&& After.TriangleCount < Before.TriangleCount
				&& After.TriangleCount <=
					FMath::CeilToInt(Before.TriangleCount * 0.5) + 1;
		case EExistingTargetWorldSemantic::SimplifyToleranceOpenGrid:
			return Before.TriangleCount == 32
				&& After.TriangleCount < Before.TriangleCount
				&& After.TriangleCount <= Before.TriangleCount
				&& FMath::Abs(After.BoundsMin.Z) <= 0.002
				&& FMath::Abs(After.BoundsMax.Z) <= 0.002;
		case EExistingTargetWorldSemantic::SimplifyOpenBoundaryPreserved:
			return Before.TriangleCount == 32
				&& After.TriangleCount < Before.TriangleCount
				&& After.TriangleCount <= 25
				&& HaveSamePositions(
					After.BoundaryPositions,
					Before.BoundaryPositions);
		case EExistingTargetWorldSemantic::SimplifyOpenBoundaryFree:
			return Before.TriangleCount == 32
				&& After.TriangleCount < Before.TriangleCount
				&& After.TriangleCount <= 5
				&& !HaveSamePositions(
					After.BoundaryPositions,
					Before.BoundaryPositions);
		case EExistingTargetWorldSemantic::SimplifyOpenBoundaryUnreachable:
			return false;
		case EExistingTargetWorldSemantic::SimpleCollisionAlignedBox:
			return Before.TriangleCount == 12
				&& Before.CollisionShapeCount == 0
				&& HaveSameGeometry(After, Before)
				&& After.CollisionShapeCount == 1
				&& After.CollisionBoxCount == 1
				&& After.CollisionSphereCount == 0
				&& After.CollisionCapsuleCount == 0
				&& After.CollisionConvexCount == 0
				&& CollisionBoundsMatchMesh(After);
		case EExistingTargetWorldSemantic::SimpleCollisionMinVolume:
			return Before.TriangleCount > 100
				&& Before.CollisionShapeCount == 0
				&& HaveSameGeometry(After, Before)
				&& After.CollisionShapeCount == 1
				&& After.CollisionBoxCount
					+ After.CollisionSphereCount
					+ After.CollisionCapsuleCount
					+ After.CollisionConvexCount == 1
				&& CollisionBoundsMatchMesh(After);
		case EExistingTargetWorldSemantic::SimpleCollisionCapsule:
			return Before.TriangleCount == 8
				&& Before.CollisionShapeCount == 0
				&& HaveSameGeometry(After, Before)
				&& After.CollisionShapeCount == 1
				&& After.CollisionBoxCount == 0
				&& After.CollisionSphereCount == 0
				&& After.CollisionCapsuleCount == 1
				&& After.CollisionConvexCount == 0
				&& CollisionBoundsMatchMesh(After);
		case EExistingTargetWorldSemantic::SimpleCollisionSphere:
			return Before.TriangleCount == 8
				&& Before.CollisionShapeCount == 0
				&& HaveSameGeometry(After, Before)
				&& After.CollisionShapeCount == 1
				&& After.CollisionBoxCount == 0
				&& After.CollisionSphereCount == 1
				&& After.CollisionCapsuleCount == 0
				&& After.CollisionConvexCount == 0
				&& CollisionBoundsMatchMesh(After);
		case EExistingTargetWorldSemantic::SimpleCollisionAlignedBoxAppend:
			return Before.TriangleCount == 12
				&& Before.CollisionShapeCount == 1
				&& Before.CollisionSphereCount == 1
				&& HaveSameGeometry(After, Before)
				&& After.CollisionShapeCount == 2
				&& After.CollisionBoxCount == 1
				&& After.CollisionSphereCount == 1
				&& After.CollisionCapsuleCount == 0
				&& After.CollisionConvexCount == 0
				&& CollisionBoundsMatchMesh(After);
		case EExistingTargetWorldSemantic::ConvexCollisionSingleHull:
			return Before.TriangleCount == 12
				&& Before.CollisionShapeCount == 0
				&& HaveSameGeometry(After, Before)
				&& After.CollisionShapeCount == 1
				&& After.CollisionBoxCount == 0
				&& After.CollisionSphereCount == 0
				&& After.CollisionCapsuleCount == 0
				&& After.CollisionConvexCount == 1
				&& CollisionBoundsMatchMesh(After);
		case EExistingTargetWorldSemantic::ConvexCollisionDecomposition:
			return Before.TriangleCount == 24
				&& Before.CollisionShapeCount == 0
				&& HaveSameGeometry(After, Before)
				&& After.CollisionShapeCount
					== After.CollisionConvexCount
				&& After.CollisionConvexCount >= 1
				&& After.CollisionConvexCount <= 2
				&& After.CollisionBoxCount == 0
				&& After.CollisionSphereCount == 0
				&& After.CollisionCapsuleCount == 0
				&& CollisionBoundsMatchMesh(After);
		case EExistingTargetWorldSemantic::NormalsFace:
			return Before.TriangleCount == 12
				&& !IsCompleteFaceNormalObservation(
					Before.NormalObservation)
				&& HaveSameGeometry(After, Before)
				&& IsCompleteFaceNormalObservation(
					After.NormalObservation)
				&& After.CollisionShapeCount
					== Before.CollisionShapeCount;
		case EExistingTargetWorldSemantic::NormalsAreaAngle:
			return Before.TriangleCount == 12
				&& !IsCompleteWeightedNormalObservation(
					Before.NormalObservation)
				&& HaveSameGeometry(After, Before)
				&& IsCompleteAreaAnglePerVertexNormalObservation(
					After.NormalObservation)
				&& After.CollisionShapeCount
					== Before.CollisionShapeCount;
		case EExistingTargetWorldSemantic::NormalsAngleThreshold:
			return Before.TriangleCount == 12
				&& !IsCompleteWeightedNormalObservation(
					Before.NormalObservation)
				&& HaveSameGeometry(After, Before)
				&& IsCompleteAngleThresholdNormalObservation(
					After.NormalObservation,
					30.0)
				&& After.CollisionShapeCount
					== Before.CollisionShapeCount;
		case EExistingTargetWorldSemantic::NormalsAreaAnglePreserve:
			return Before.TriangleCount == 12
				&& HaveSameGeometry(After, Before)
				&& HasCompletePreservedHardEdgeObservation(
					Before.NormalObservation,
					After.NormalObservation)
				&& After.CollisionShapeCount
					== Before.CollisionShapeCount;
		default:
			return false;
		}
	}

	bool RunExistingTargetWorldArtifactCase(
		FAutomationTestBase& Test,
		const FExistingTargetWorldCase& TestCase)
	{
		using namespace UE::UAI::Module::CommandRegistration::Active;

		FOwnedPublicWorld Owned;
		if (!Test.TestTrue(
				*(TestCase.Scope + TEXT(" creates an owned persisted world")),
				Owned.IsValid()))
		{
			return false;
		}
		UWorld* const SourceWorld = Owned.World;
		UE::UAI::Composition::SimpleAuthoring::OperationHost::
			SetAutomationWorldOverride(SourceWorld, true);
		bool bWorldOverrideActive = true;
		ON_SCOPE_EXIT
		{
			if (bWorldOverrideActive)
			{
				UE::UAI::Composition::SimpleAuthoring::OperationHost::
					SetAutomationWorldOverride(nullptr, true);
			}
		};

		FActorSpawnParameters SpawnParameters;
		SpawnParameters.Name = FName(*TestCase.ActorName);
		SpawnParameters.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		SpawnParameters.ObjectFlags |= RF_Transactional;
		ADynamicMeshActor* const Actor =
			SourceWorld->SpawnActor<ADynamicMeshActor>(
				FVector::ZeroVector,
				FRotator::ZeroRotator,
				SpawnParameters);
		if (!Test.TestNotNull(
				*(TestCase.Scope + TEXT(" creates an owned DynamicMeshActor")),
				Actor))
		{
			return false;
		}
		Actor->SetActorLabel(TestCase.ActorName);
		UDynamicMesh* const DynamicMesh = Actor->GetDynamicMeshComponent()
			? Actor->GetDynamicMeshComponent()->GetDynamicMesh()
			: nullptr;
		if (!Test.TestNotNull(
				*(TestCase.Scope + TEXT(" exposes an owned DynamicMesh")),
				DynamicMesh))
		{
			return false;
		}
		DynamicMesh->EditMesh(
			[&TestCase](FDynamicMesh3& Mesh)
			{
				switch (TestCase.Seed)
				{
				case EExistingTargetWorldSeed::OpenGrid:
					BuildOpenGrid(Mesh);
					break;
				case EExistingTargetWorldSeed::SphereLike:
					BuildOctahedron(Mesh, 50.0, 50.0);
					break;
				case EExistingTargetWorldSeed::DenseSphereLike:
					BuildDenseSphere(Mesh);
					break;
				case EExistingTargetWorldSeed::CapsuleLike:
					BuildOctahedron(Mesh, 50.0, 100.0);
					break;
				case EExistingTargetWorldSeed::DisconnectedCubes:
					BuildTwoDisconnectedCubes(Mesh);
					break;
				case EExistingTargetWorldSeed::ClosedCube:
				default:
					BuildCube(Mesh);
					break;
				}
			});
		if (TestCase.Semantic ==
				EExistingTargetWorldSemantic::
					SimpleCollisionAlignedBoxAppend
			&& !Test.TestTrue(
				*(TestCase.Scope + TEXT(" seeds an existing sphere collision")),
				SeedSphereCollision(*Actor)))
		{
			return false;
		}
		if (TestCase.Semantic ==
			EExistingTargetWorldSemantic::NormalsAreaAnglePreserve)
		{
			SeedMixedHardEdgeNormals(*DynamicMesh);
		}
		const FGuid ActorGuid = Actor->GetActorGuid();
		if (!Test.TestTrue(
				*(TestCase.Scope + TEXT(" has a durable actor identity")),
				ActorGuid.IsValid()))
		{
			return false;
		}
		FMeshObservation BeforeObservation;
		const bool bBaselineReadable =
			ReadMeshObservation(*Actor, BeforeObservation);
		bool bBaselineMatches = false;
		switch (TestCase.Seed)
		{
		case EExistingTargetWorldSeed::OpenGrid:
			bBaselineMatches = BeforeObservation.TriangleCount == 32
				&& BeforeObservation.BoundaryEdgeCount > 0
				&& BeforeObservation.BoundaryPositions.Num() == 16;
			break;
		case EExistingTargetWorldSeed::SphereLike:
		case EExistingTargetWorldSeed::CapsuleLike:
			bBaselineMatches = BeforeObservation.TriangleCount == 8
				&& BeforeObservation.BoundaryEdgeCount == 0;
			break;
		case EExistingTargetWorldSeed::DenseSphereLike:
			bBaselineMatches = BeforeObservation.TriangleCount > 100
				&& BeforeObservation.BoundaryEdgeCount == 0;
			break;
		case EExistingTargetWorldSeed::DisconnectedCubes:
			bBaselineMatches = BeforeObservation.TriangleCount == 24
				&& BeforeObservation.BoundaryEdgeCount == 0;
			break;
		case EExistingTargetWorldSeed::ClosedCube:
		default:
			bBaselineMatches = BeforeObservation.TriangleCount == 12
				&& BeforeObservation.BoundaryEdgeCount == 0;
			break;
		}
		if (TestCase.Semantic ==
			EExistingTargetWorldSemantic::NormalsAreaAnglePreserve)
		{
			bBaselineMatches = bBaselineMatches
				&& HasCompleteMixedHardEdgeSeed(
					BeforeObservation.NormalObservation);
		}
		if (!Test.TestTrue(
				*(TestCase.Scope + TEXT(" reads the exact seed baseline")),
				bBaselineReadable && bBaselineMatches))
		{
			return false;
		}

		TOptional<FUeAgentStateRevision> BeforeRevision;
		FExecutionMutationError RevisionError;
		const FString TargetRef = Actor->GetPathName();
		if (!Test.TestTrue(
				*(TestCase.Scope + TEXT(" reads the exact target revision")),
				TryReadModelingGeometryCarrierRevision(
					TargetRef,
					*Actor,
					BeforeRevision,
					RevisionError)
					&& BeforeRevision.IsSet()))
		{
			Test.AddError(
				RevisionError.GetCode() + TEXT(":")
				+ RevisionError.GetDetail());
			return false;
		}

		TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
		TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
		FString CompositionError;
		if (!Test.TestTrue(
				*(TestCase.Scope + TEXT(" freezes the active public composition")),
				TryComposeProcessActiveCommandComposition(
					Registry,
					Dispatch,
					CompositionError))
			|| !Registry.IsValid()
			|| !Dispatch.IsValid())
		{
			Test.AddError(CompositionError);
			return false;
		}
		const FUeAgentCommandResult Result =
			UE::UAI::Tests::PublicCommand::SubmitActiveCommand(
				Registry,
				Dispatch,
				TestCase.CommandId,
				BuildExistingTargetWorldParams(
					TestCase,
					TargetRef,
					BeforeRevision.GetValue()),
				TestCase.RequestId);
		if (!TestCase.ExpectedErrorCode.IsEmpty())
		{
			Test.TestFalse(
				*(TestCase.Scope + TEXT(" rejects the unreachable public mutation")),
				Result.IsOk());
			Test.TestEqual(
				*(TestCase.Scope + TEXT(" returns the exact typed failure")),
				ReadCommandErrorCode(Result),
				TestCase.ExpectedErrorCode);
			FMeshObservation RejectedObservation;
			Test.TestTrue(
				*(TestCase.Scope + TEXT(" leaves the live geometry unchanged")),
				ReadMeshObservation(*Actor, RejectedObservation)
					&& RejectedObservation.TriangleCount
						== BeforeObservation.TriangleCount
					&& RejectedObservation.BoundaryEdgeCount
						== BeforeObservation.BoundaryEdgeCount
					&& HaveSamePositions(
						RejectedObservation.BoundaryPositions,
						BeforeObservation.BoundaryPositions)
					&& IsNearlyEqualVector(
						RejectedObservation.BoundsMin,
						BeforeObservation.BoundsMin)
					&& IsNearlyEqualVector(
						RejectedObservation.BoundsMax,
						BeforeObservation.BoundsMax));
			TOptional<FUeAgentStateRevision> RejectedRevision;
			RevisionError = {};
			Test.TestTrue(
				*(TestCase.Scope + TEXT(" leaves the carrier revision unchanged")),
				TryReadModelingGeometryCarrierRevision(
					TargetRef,
					*Actor,
					RejectedRevision,
					RevisionError)
					&& RejectedRevision.IsSet()
					&& RejectedRevision.GetValue()
						== BeforeRevision.GetValue());
			Test.TestFalse(
				*(TestCase.Scope + TEXT(" creates no durable map artifact")),
				IFileManager::Get().FileExists(*Owned.HeaderPath));
			return !Test.HasAnyErrors();
		}
		if (!Test.TestTrue(
				*(TestCase.Scope + TEXT(" public mutation succeeds")),
				Result.IsOk()))
		{
			Test.AddError(Result.GetErrorJson());
			FMeshObservation FailureObservation;
			if (ReadMeshObservation(*Actor, FailureObservation))
			{
				Test.AddInfo(
					TestCase.Scope + TEXT(" collision diagnostic: ")
						+ DescribeCollisionObservation(FailureObservation));
			}
			return false;
		}
		Test.TestTrue(
			*(TestCase.Scope + TEXT(" C9 names isolated persisted-world readback")),
			IsVerifiedWorldArtifactOutcome(Result.GetPayloadJson()));
		Test.TestTrue(
			*(TestCase.Scope + TEXT(" creates a durable map artifact")),
			IFileManager::Get().FileExists(*Owned.HeaderPath));

		FMeshObservation SourceAfterObservation;
		Test.TestTrue(
			*(TestCase.Scope + TEXT(" applies the requested source geometry semantic")),
			ReadMeshObservation(*Actor, SourceAfterObservation)
				&& MatchesExistingTargetWorldSemantic(
					TestCase,
					BeforeObservation,
					SourceAfterObservation));
		TOptional<FUeAgentStateRevision> SourceAfterRevision;
		RevisionError = {};
		Test.TestTrue(
			*(TestCase.Scope + TEXT(" advances the source carrier revision")),
			TryReadModelingGeometryCarrierRevision(
				TargetRef,
				*Actor,
				SourceAfterRevision,
				RevisionError)
				&& SourceAfterRevision.IsSet()
				&& SourceAfterRevision.GetValue()
					!= BeforeRevision.GetValue());

		UE::UAI::Composition::SimpleAuthoring::OperationHost::
			SetAutomationWorldOverride(nullptr, true);
		bWorldOverrideActive = false;
		const TWeakObjectPtr<ADynamicMeshActor> OriginalActor(Actor);
		FString ReloadError;
		if (!Test.TestTrue(
				*(TestCase.Scope + TEXT(" reloads the source world from disk")),
				Owned.TryReload(ReloadError)))
		{
			Test.AddError(ReloadError);
			return false;
		}
		Test.TestFalse(
			*(TestCase.Scope + TEXT(" does not reuse the source actor")),
			OriginalActor.IsValid());
		ADynamicMeshActor* const ReloadedActor = FindActorByGuid(
			*Owned.World,
			ActorGuid);
		if (!Test.TestNotNull(
				*(TestCase.Scope + TEXT(" reloads the exact actor GUID")),
				ReloadedActor))
		{
			return false;
		}
		Test.TestEqual(
			*(TestCase.Scope + TEXT(" preserves the actor label")),
			ReloadedActor->GetActorLabel(),
			TestCase.ActorName);
		Test.TestEqual(
			*(TestCase.Scope + TEXT(" preserves the target path")),
			ReloadedActor->GetPathName(),
			TargetRef);

		FMeshObservation ReloadedObservation;
		if (!Test.TestTrue(
				*(TestCase.Scope + TEXT(" reloads readable persisted geometry")),
				ReadMeshObservation(*ReloadedActor, ReloadedObservation)))
		{
			return false;
		}
		Test.TestTrue(
			*(TestCase.Scope + TEXT(" preserves the requested geometry semantic")),
			MatchesExistingTargetWorldSemantic(
				TestCase,
				BeforeObservation,
				ReloadedObservation));
		Test.TestEqual(
			*(TestCase.Scope + TEXT(" persists the exact source triangle count")),
			ReloadedObservation.TriangleCount,
			SourceAfterObservation.TriangleCount);
		Test.TestEqual(
			*(TestCase.Scope + TEXT(" persists the exact source boundary count")),
			ReloadedObservation.BoundaryEdgeCount,
			SourceAfterObservation.BoundaryEdgeCount);
		Test.TestTrue(
			*(TestCase.Scope + TEXT(" persists the exact source boundary positions")),
			HaveSamePositions(
				ReloadedObservation.BoundaryPositions,
				SourceAfterObservation.BoundaryPositions));
		Test.TestTrue(
			*(TestCase.Scope + TEXT(" persists the exact source bounds")),
			IsNearlyEqualVector(
				ReloadedObservation.BoundsMin,
				SourceAfterObservation.BoundsMin)
				&& IsNearlyEqualVector(
					ReloadedObservation.BoundsMax,
					SourceAfterObservation.BoundsMax));
		Test.TestEqual(
			*(TestCase.Scope + TEXT(" persists the exact collision shape count")),
			ReloadedObservation.CollisionShapeCount,
			SourceAfterObservation.CollisionShapeCount);
		Test.TestEqual(
			*(TestCase.Scope + TEXT(" persists the exact collision box count")),
			ReloadedObservation.CollisionBoxCount,
			SourceAfterObservation.CollisionBoxCount);
		Test.TestEqual(
			*(TestCase.Scope + TEXT(" persists the exact collision sphere count")),
			ReloadedObservation.CollisionSphereCount,
			SourceAfterObservation.CollisionSphereCount);
		Test.TestEqual(
			*(TestCase.Scope + TEXT(" persists the exact collision capsule count")),
			ReloadedObservation.CollisionCapsuleCount,
			SourceAfterObservation.CollisionCapsuleCount);
		Test.TestEqual(
			*(TestCase.Scope + TEXT(" persists the exact collision convex count")),
			ReloadedObservation.CollisionConvexCount,
			SourceAfterObservation.CollisionConvexCount);
		Test.TestEqual(
			*(TestCase.Scope + TEXT(" persists collision bounds validity")),
			ReloadedObservation.bCollisionBoundsValid,
			SourceAfterObservation.bCollisionBoundsValid);
		if (SourceAfterObservation.bCollisionBoundsValid)
		{
			Test.TestTrue(
				*(TestCase.Scope + TEXT(" persists the exact collision bounds")),
				IsNearlyEqualVector(
					ReloadedObservation.CollisionBoundsMin,
					SourceAfterObservation.CollisionBoundsMin)
					&& IsNearlyEqualVector(
						ReloadedObservation.CollisionBoundsMax,
						SourceAfterObservation.CollisionBoundsMax));
		}
		TOptional<FUeAgentStateRevision> ReloadedRevision;
		RevisionError = {};
		Test.TestTrue(
			*(TestCase.Scope + TEXT(" persists the advanced carrier revision")),
			TryReadModelingGeometryCarrierRevision(
				TargetRef,
				*ReloadedActor,
				ReloadedRevision,
				RevisionError)
				&& ReloadedRevision.IsSet()
				&& SourceAfterRevision.IsSet()
				&& ReloadedRevision.GetValue()
					== SourceAfterRevision.GetValue());
		return !Test.HasAnyErrors();
	}

	const TArray<FExistingTargetWorldCase>&
	GetExistingTargetSimplifyExpansionCases()
	{
		static const TArray<FExistingTargetWorldCase> Cases = {
			{
				TEXT("Mode.ToleranceOpenGrid"),
				TEXT("T50ModelingWorldCarrierSimplifyToleranceTarget"),
				TEXT("mesh.topology.simplify"),
				TEXT("T50PublicDynamicMeshWorldArtifactSimplifyTolerance"),
				EExistingTargetWorldSemantic::SimplifyToleranceOpenGrid,
				EExistingTargetWorldSeed::OpenGrid,
			},
			{
				TEXT("Policy.OpenBoundaryPreserved"),
				TEXT("T50ModelingWorldCarrierSimplifyBoundaryPreservedTarget"),
				TEXT("mesh.topology.simplify"),
				TEXT("T50PublicDynamicMeshWorldArtifactSimplifyBoundaryPreserved"),
				EExistingTargetWorldSemantic::SimplifyOpenBoundaryPreserved,
				EExistingTargetWorldSeed::OpenGrid,
			},
			{
				TEXT("Policy.OpenBoundaryFree"),
				TEXT("T50ModelingWorldCarrierSimplifyBoundaryFreeTarget"),
				TEXT("mesh.topology.simplify"),
				TEXT("T50PublicDynamicMeshWorldArtifactSimplifyBoundaryFree"),
				EExistingTargetWorldSemantic::SimplifyOpenBoundaryFree,
				EExistingTargetWorldSeed::OpenGrid,
			},
			{
				TEXT("Policy.OpenBoundaryUnreachableRejected"),
				TEXT("T50ModelingWorldCarrierSimplifyBoundaryRejectedTarget"),
				TEXT("mesh.topology.simplify"),
				TEXT("T50PublicDynamicMeshWorldArtifactSimplifyBoundaryRejected"),
				EExistingTargetWorldSemantic::SimplifyOpenBoundaryUnreachable,
				EExistingTargetWorldSeed::OpenGrid,
				TEXT("modeling_geometry_simplify_target_unreachable"),
			},
		};
		return Cases;
	}

	const FExistingTargetWorldCase* FindExistingTargetSimplifyExpansionCase(
		const FString& Scope)
	{
		return GetExistingTargetSimplifyExpansionCases().FindByPredicate(
			[&](const FExistingTargetWorldCase& Candidate)
			{
				return Candidate.Scope == Scope;
			});
	}

	const TArray<FExistingTargetWorldCase>&
	GetExistingTargetCollisionExpansionCases()
	{
		static const TArray<FExistingTargetWorldCase> Cases = {
			{
				TEXT("Simple.MinVolumeNonBox"),
				TEXT("T50ModelingWorldCarrierMinVolumeCollisionTarget"),
				TEXT("mesh.collision.simple.generate"),
				TEXT("T50PublicDynamicMeshWorldArtifactMinVolumeCollision"),
				EExistingTargetWorldSemantic::SimpleCollisionMinVolume,
				EExistingTargetWorldSeed::DenseSphereLike,
			},
			{
				TEXT("Simple.CapsuleElongated"),
				TEXT("T50ModelingWorldCarrierCapsuleCollisionTarget"),
				TEXT("mesh.collision.simple.generate"),
				TEXT("T50PublicDynamicMeshWorldArtifactCapsuleCollision"),
				EExistingTargetWorldSemantic::SimpleCollisionCapsule,
				EExistingTargetWorldSeed::CapsuleLike,
			},
			{
				TEXT("Simple.SphereLike"),
				TEXT("T50ModelingWorldCarrierSphereCollisionTarget"),
				TEXT("mesh.collision.simple.generate"),
				TEXT("T50PublicDynamicMeshWorldArtifactSphereCollision"),
				EExistingTargetWorldSemantic::SimpleCollisionSphere,
				EExistingTargetWorldSeed::SphereLike,
			},
			{
				TEXT("Convex.SingleHull"),
				TEXT("T50ModelingWorldCarrierSingleHullCollisionTarget"),
				TEXT("mesh.collision.convex.generate"),
				TEXT("T50PublicDynamicMeshWorldArtifactSingleHullCollision"),
				EExistingTargetWorldSemantic::ConvexCollisionSingleHull,
				EExistingTargetWorldSeed::ClosedCube,
			},
			{
				TEXT("Convex.DecompositionTwoComponents"),
				TEXT("T50ModelingWorldCarrierDecompositionCollisionTarget"),
				TEXT("mesh.collision.convex.generate"),
				TEXT("T50PublicDynamicMeshWorldArtifactDecompositionCollision"),
				EExistingTargetWorldSemantic::ConvexCollisionDecomposition,
				EExistingTargetWorldSeed::DisconnectedCubes,
			},
			{
				TEXT("Policy.AppendPreservesExistingSphere"),
				TEXT("T50ModelingWorldCarrierAppendCollisionTarget"),
				TEXT("mesh.collision.simple.generate"),
				TEXT("T50PublicDynamicMeshWorldArtifactAppendCollision"),
				EExistingTargetWorldSemantic::SimpleCollisionAlignedBoxAppend,
				EExistingTargetWorldSeed::ClosedCube,
			},
		};
		return Cases;
	}

	const FExistingTargetWorldCase* FindExistingTargetCollisionExpansionCase(
		const FString& Scope)
	{
		return GetExistingTargetCollisionExpansionCases().FindByPredicate(
			[&](const FExistingTargetWorldCase& Candidate)
			{
				return Candidate.Scope == Scope;
			});
	}

	const TArray<FExistingTargetWorldCase>&
	GetExistingTargetNormalsExpansionCases()
	{
		static const TArray<FExistingTargetWorldCase> Cases = {
			{
				TEXT("Mode.AreaAngleWeighted"),
				TEXT("T50ModelingWorldCarrierNormalsAreaAngleTarget"),
				TEXT("mesh.attribute.normals.recompute"),
				TEXT("T50PublicDynamicMeshWorldArtifactNormalsAreaAngle"),
				EExistingTargetWorldSemantic::NormalsAreaAngle,
			},
			{
				TEXT("Mode.AngleThreshold30"),
				TEXT("T50ModelingWorldCarrierNormalsThresholdTarget"),
				TEXT("mesh.attribute.normals.recompute"),
				TEXT("T50PublicDynamicMeshWorldArtifactNormalsThreshold"),
				EExistingTargetWorldSemantic::NormalsAngleThreshold,
			},
			{
				TEXT("Policy.PreserveMixedHardEdges"),
				TEXT("T50ModelingWorldCarrierNormalsPreserveTarget"),
				TEXT("mesh.attribute.normals.recompute"),
				TEXT("T50PublicDynamicMeshWorldArtifactNormalsPreserve"),
				EExistingTargetWorldSemantic::NormalsAreaAnglePreserve,
			},
		};
		return Cases;
	}

	const FExistingTargetWorldCase* FindExistingTargetNormalsExpansionCase(
		const FString& Scope)
	{
		return GetExistingTargetNormalsExpansionCases().FindByPredicate(
			[&](const FExistingTargetWorldCase& Candidate)
			{
				return Candidate.Scope == Scope;
			});
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentModelingGeometryPublicDynamicMeshWorldArtifactTest,
	"UeAgentInterface.DomainAdapters.ModelingGeometryEffect.E4.PublicProduct."
		"DynamicMeshTopologyWorldArtifactReadback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentModelingGeometryPublicDynamicMeshWorldArtifactTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Tests::ModelingGeometryPublicWorldCarrier;
	(void)Parameters;
	return RunExistingTargetWorldArtifactCase(
		*this,
		{
			TEXT("Subdivide"),
			TEXT("T50ModelingWorldCarrierSubdivideTarget"),
			TEXT("mesh.topology.subdivide"),
			TEXT("T50PublicDynamicMeshWorldArtifactSubdivide"),
			EExistingTargetWorldSemantic::SubdivideUniformLevelOne,
		});
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentModelingGeometryPublicDynamicMeshSimplifyWorldArtifactTest,
	"UeAgentInterface.DomainAdapters.ModelingGeometryEffect.E4.PublicProduct."
		"DynamicMeshSimplifyWorldArtifactReadback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentModelingGeometryPublicDynamicMeshSimplifyWorldArtifactTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Tests::ModelingGeometryPublicWorldCarrier;
	(void)Parameters;
	return RunExistingTargetWorldArtifactCase(
		*this,
		{
			TEXT("SimplifyTriangleRatioHalf"),
			TEXT("T50ModelingWorldCarrierSimplifyTarget"),
			TEXT("mesh.topology.simplify"),
			TEXT("T50PublicDynamicMeshWorldArtifactSimplifyRatio"),
			EExistingTargetWorldSemantic::SimplifyTriangleRatioHalf,
		});
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(
	FUeAgentModelingGeometryPublicDynamicMeshSimplifyWorldArtifactExpansionTest,
	"UeAgentInterface.DomainAdapters.ModelingGeometryEffect.E4.PublicProduct."
		"DynamicMeshSimplifyWorldArtifactExpansion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

void
FUeAgentModelingGeometryPublicDynamicMeshSimplifyWorldArtifactExpansionTest::
	GetTests(
		TArray<FString>& OutBeautifiedNames,
		TArray<FString>& OutTestCommands) const
{
	using namespace UE::UAI::Tests::ModelingGeometryPublicWorldCarrier;
	for (const FExistingTargetWorldCase& TestCase
		: GetExistingTargetSimplifyExpansionCases())
	{
		OutBeautifiedNames.Add(TestCase.Scope);
		OutTestCommands.Add(TestCase.Scope);
	}
}

bool
FUeAgentModelingGeometryPublicDynamicMeshSimplifyWorldArtifactExpansionTest::
	RunTest(const FString& Parameters)
{
	using namespace UE::UAI::Tests::ModelingGeometryPublicWorldCarrier;
	const FExistingTargetWorldCase* const TestCase =
		FindExistingTargetSimplifyExpansionCase(Parameters);
	if (!TestNotNull(TEXT("the simplify expansion case is registered"), TestCase))
	{
		return false;
	}
	return RunExistingTargetWorldArtifactCase(*this, *TestCase);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentModelingGeometryPublicDynamicMeshSimpleCollisionWorldArtifactTest,
	"UeAgentInterface.DomainAdapters.ModelingGeometryEffect.E4.PublicProduct."
		"DynamicMeshSimpleCollisionWorldArtifactReadback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool
FUeAgentModelingGeometryPublicDynamicMeshSimpleCollisionWorldArtifactTest::
	RunTest(const FString& Parameters)
{
	using namespace UE::UAI::Tests::ModelingGeometryPublicWorldCarrier;
	(void)Parameters;
	return RunExistingTargetWorldArtifactCase(
		*this,
		{
			TEXT("SimpleCollisionAlignedBox"),
			TEXT("T50ModelingWorldCarrierSimpleCollisionTarget"),
			TEXT("mesh.collision.simple.generate"),
			TEXT("T50PublicDynamicMeshWorldArtifactSimpleCollision"),
			EExistingTargetWorldSemantic::SimpleCollisionAlignedBox,
		});
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentModelingGeometryPublicDynamicMeshNormalsFaceWorldArtifactTest,
	"UeAgentInterface.DomainAdapters.ModelingGeometryEffect.E4.PublicProduct."
		"DynamicMeshNormalsFaceWorldArtifactReadback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool
FUeAgentModelingGeometryPublicDynamicMeshNormalsFaceWorldArtifactTest::
	RunTest(const FString& Parameters)
{
	using namespace UE::UAI::Tests::ModelingGeometryPublicWorldCarrier;
	(void)Parameters;
	return RunExistingTargetWorldArtifactCase(
		*this,
		{
			TEXT("NormalsFace"),
			TEXT("T50ModelingWorldCarrierNormalsFaceTarget"),
			TEXT("mesh.attribute.normals.recompute"),
			TEXT("T50PublicDynamicMeshWorldArtifactNormalsFace"),
			EExistingTargetWorldSemantic::NormalsFace,
		});
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(
	FUeAgentModelingGeometryPublicDynamicMeshNormalsWorldArtifactExpansionTest,
	"UeAgentInterface.DomainAdapters.ModelingGeometryEffect.E4.PublicProduct."
		"DynamicMeshNormalsWorldArtifactExpansion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

void
FUeAgentModelingGeometryPublicDynamicMeshNormalsWorldArtifactExpansionTest::
	GetTests(
		TArray<FString>& OutBeautifiedNames,
		TArray<FString>& OutTestCommands) const
{
	using namespace UE::UAI::Tests::ModelingGeometryPublicWorldCarrier;
	for (const FExistingTargetWorldCase& TestCase
		: GetExistingTargetNormalsExpansionCases())
	{
		OutBeautifiedNames.Add(TestCase.Scope);
		OutTestCommands.Add(TestCase.Scope);
	}
}

bool
FUeAgentModelingGeometryPublicDynamicMeshNormalsWorldArtifactExpansionTest::
	RunTest(const FString& Parameters)
{
	using namespace UE::UAI::Tests::ModelingGeometryPublicWorldCarrier;
	const FExistingTargetWorldCase* const TestCase =
		FindExistingTargetNormalsExpansionCase(Parameters);
	if (!TestNotNull(TEXT("the normals expansion case is registered"), TestCase))
	{
		return false;
	}
	return RunExistingTargetWorldArtifactCase(*this, *TestCase);
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(
	FUeAgentModelingGeometryPublicDynamicMeshCollisionWorldArtifactExpansionTest,
	"UeAgentInterface.DomainAdapters.ModelingGeometryEffect.E4.PublicProduct."
		"DynamicMeshCollisionWorldArtifactExpansion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

void
FUeAgentModelingGeometryPublicDynamicMeshCollisionWorldArtifactExpansionTest::
	GetTests(
		TArray<FString>& OutBeautifiedNames,
		TArray<FString>& OutTestCommands) const
{
	using namespace UE::UAI::Tests::ModelingGeometryPublicWorldCarrier;
	for (const FExistingTargetWorldCase& TestCase
		: GetExistingTargetCollisionExpansionCases())
	{
		OutBeautifiedNames.Add(TestCase.Scope);
		OutTestCommands.Add(TestCase.Scope);
	}
}

bool
FUeAgentModelingGeometryPublicDynamicMeshCollisionWorldArtifactExpansionTest::
	RunTest(const FString& Parameters)
{
	using namespace UE::UAI::Tests::ModelingGeometryPublicWorldCarrier;
	const FExistingTargetWorldCase* const TestCase =
		FindExistingTargetCollisionExpansionCase(Parameters);
	if (!TestNotNull(TEXT("the collision expansion case is registered"), TestCase))
	{
		return false;
	}
	return RunExistingTargetWorldArtifactCase(*this, *TestCase);
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(
	FUeAgentModelingGeometryPublicPrimitiveWorldArtifactTest,
	"UeAgentInterface.DomainAdapters.ModelingGeometryEffect.E4.PublicProduct."
		"PrimitiveWorldArtifactReadback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

void FUeAgentModelingGeometryPublicPrimitiveWorldArtifactTest::GetTests(
	TArray<FString>& OutBeautifiedNames,
	TArray<FString>& OutTestCommands) const
{
	using namespace UE::UAI::Tests::ModelingGeometryPublicWorldCarrier;
	for (const FPrimitiveWorldCase& TestCase : GetPrimitiveWorldCases())
	{
		OutBeautifiedNames.Add(TestCase.BeautifiedName);
		OutTestCommands.Add(TestCase.Key);
	}
}

bool FUeAgentModelingGeometryPublicPrimitiveWorldArtifactTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Module::CommandRegistration::Active;
	using namespace UE::UAI::Tests::ModelingGeometryPublicWorldCarrier;
	const FPrimitiveWorldCase* const TestCase = FindPrimitiveWorldCase(
		Parameters);
	if (!TestNotNull(TEXT("the primitive case is registered"), TestCase))
	{
		return false;
	}

	FOwnedPublicWorld Owned;
	if (!TestTrue(TEXT("the owned persisted world is valid"), Owned.IsValid()))
	{
		return false;
	}
	UWorld* const SourceWorld = Owned.World;
	UE::UAI::Composition::SimpleAuthoring::OperationHost::
		SetAutomationWorldOverride(SourceWorld, true);
	bool bWorldOverrideActive = true;
	ON_SCOPE_EXIT
	{
		if (bWorldOverrideActive)
		{
			UE::UAI::Composition::SimpleAuthoring::OperationHost::
				SetAutomationWorldOverride(nullptr, true);
		}
	};

	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	FString CompositionError;
	if (!TestTrue(
		TEXT("the active public composition freezes"),
		TryComposeProcessActiveCommandComposition(
			Registry,
			Dispatch,
			CompositionError))
		|| !Registry.IsValid()
		|| !Dispatch.IsValid())
	{
		AddError(CompositionError);
		return false;
	}

	const FString TargetRef = FString::Printf(
		TEXT("T50ModelingPrimitive_%s"),
		*TestCase->Key);
	const FUeAgentCommandResult Result =
		UE::UAI::Tests::PublicCommand::SubmitActiveCommand(
			Registry,
			Dispatch,
			TestCase->CommandId,
			BuildPrimitiveParams(*TestCase, TargetRef),
			TEXT("T50PublicPrimitiveWorldArtifact_") + TestCase->Key);
	const bool bPublicCreationSucceeded = TestTrue(
		TEXT("the public primitive creation succeeds"),
		Result.IsOk());
	if (!bPublicCreationSucceeded)
	{
		AddError(Result.GetErrorJson());
	}
	else
	{
		TestTrue(
			TEXT("C9 names isolated persisted-world readback"),
			IsVerifiedWorldArtifactOutcome(Result.GetPayloadJson()));
	}
	TestTrue(
		TEXT("the durable map artifact exists"),
		IFileManager::Get().FileExists(*Owned.HeaderPath));

	ADynamicMeshActor* const CreatedActor = FindActorByLabel(
		*SourceWorld,
		TargetRef);
	if (!TestNotNull(
		TEXT("the public command creates one exact DynamicMeshActor"),
		CreatedActor))
	{
		return false;
	}
	TestTrue(
		TEXT("the primitive has a durable UE actor identity"),
		CreatedActor->GetActorGuid().IsValid());
	AddInfo(FString::Printf(
		TEXT("source primitive transform location=%s rotation=%s scale=%s"),
		*CreatedActor->GetActorLocation().ToString(),
		*CreatedActor->GetActorQuat().ToString(),
		*CreatedActor->GetActorScale3D().ToString()));
	TestTrue(
		TEXT("the source actor preserves the requested placement"),
		IsNearlyEqualVector(
			CreatedActor->GetActorLocation(),
			TestCase->ExpectedLocation));
	TestTrue(
		TEXT("the source actor preserves the requested rotation"),
		CreatedActor->GetActorQuat().Equals(
			ExpectedPrimitiveRotation(),
			1.e-4));
	TestTrue(
		TEXT("the source actor preserves the requested scale"),
		IsNearlyEqualVector(
			CreatedActor->GetActorScale3D(),
			ExpectedPrimitiveScale()));
	FMeshObservation SourceObservation;
	if (!TestTrue(
		TEXT("the source primitive has readable geometry"),
		ReadMeshObservation(*CreatedActor, SourceObservation)))
	{
		return false;
	}
	TestTrue(
		TEXT("the source primitive has the requested local bounds"),
		IsNearlyEqualVector(
			SourceObservation.BoundsMax - SourceObservation.BoundsMin,
			TestCase->ExpectedBoundsSize));

	UE::UAI::Composition::SimpleAuthoring::OperationHost::
		SetAutomationWorldOverride(nullptr, true);
	bWorldOverrideActive = false;
	const TWeakObjectPtr<ADynamicMeshActor> OriginalActor(CreatedActor);
	FString ReloadError;
	if (!TestTrue(
		TEXT("the source world unloads and reloads from disk"),
		Owned.TryReload(ReloadError)))
	{
		AddError(ReloadError);
		return false;
	}
	TestFalse(
		TEXT("the external reload does not reuse the source actor"),
		OriginalActor.IsValid());
	ADynamicMeshActor* const ReloadedActor = FindActorByLabel(
		*Owned.World,
		TargetRef);
	if (!TestNotNull(
		TEXT("the exact primitive actor is independently reloadable"),
		ReloadedActor))
	{
		return false;
	}
	AddInfo(FString::Printf(
		TEXT("reloaded primitive transform location=%s rotation=%s scale=%s"),
		*ReloadedActor->GetActorLocation().ToString(),
		*ReloadedActor->GetActorQuat().ToString(),
		*ReloadedActor->GetActorScale3D().ToString()));
	TestTrue(
		TEXT("the reloaded actor preserves the requested placement"),
		IsNearlyEqualVector(
			ReloadedActor->GetActorLocation(),
			TestCase->ExpectedLocation));
	TestTrue(
		TEXT("the reloaded actor preserves the requested rotation"),
		ReloadedActor->GetActorQuat().Equals(
			ExpectedPrimitiveRotation(),
			1.e-4));
	TestTrue(
		TEXT("the reloaded actor preserves the requested scale"),
		IsNearlyEqualVector(
			ReloadedActor->GetActorScale3D(),
			ExpectedPrimitiveScale()));
	FMeshObservation ReloadedObservation;
	if (!TestTrue(
		TEXT("the reloaded primitive has readable geometry"),
		ReadMeshObservation(*ReloadedActor, ReloadedObservation)))
	{
		return false;
	}
	TestTrue(
		TEXT("the reloaded primitive preserves its local bounds"),
		IsNearlyEqualVector(
			ReloadedObservation.BoundsMax - ReloadedObservation.BoundsMin,
			TestCase->ExpectedBoundsSize));
	TestEqual(
		TEXT("the reloaded primitive preserves its exact triangle count"),
		ReloadedObservation.TriangleCount,
		SourceObservation.TriangleCount);
	return !HasAnyErrors();
}

#endif

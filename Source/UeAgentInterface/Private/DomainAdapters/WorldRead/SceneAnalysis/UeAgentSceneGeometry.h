#pragma once

#include "CoreMinimal.h"
#include "Curve/GeneralPolygon2.h"
#include "IndexTypes.h"
#include "Spatial/MeshAABBTree3.h"
#include "Spatial/FastWinding.h"

namespace UE::UAI::SceneAnalysis
{
    // Plain immutable geometry after capture. No UObject is read by the worker.
    struct FTriangleMesh
    {
        TArray<FVector3d> Vertices;
        TArray<UE::Geometry::FIndex3i> Triangles;
        TArray<int32> TriangleIds;
        uint64 Revision = 1;
        uint64 GetChangeStamp() const { return Revision; }
        int32 TriangleCount() const { return Triangles.Num(); }
        int32 MaxTriangleID() const { return Triangles.Num(); }
        bool IsTriangle(int32 Id) const { return Triangles.IsValidIndex(Id); }
        const TArray<int32>& TriangleIndicesItr() const { return TriangleIds; }
        UE::Geometry::FIndex3i GetTriangle(int32 Id) const { return Triangles[Id]; }
        FVector3d GetVertex(int32 Id) const { return Vertices[Id]; }
        void GetTriVertices(int32 Id, FVector3d& A, FVector3d& B, FVector3d& C) const
        {
            const auto T = Triangles[Id]; A = Vertices[T.A]; B = Vertices[T.B]; C = Vertices[T.C];
        }
    };

    struct UEAGENTINTERFACE_API FSurfaceFrame
    {
        FVector3d Origin = FVector3d::ZeroVector;
        FVector3d Normal = FVector3d(0, 0, 1);
        FVector3d U = FVector3d(1, 0, 0);
        FVector3d V = FVector3d(0, 1, 0);
        FVector2d Project(const FVector3d& Point) const;
        FVector3d Lift(const FVector2d& Point) const;
    };

    struct FPlanarSurface
    {
        FSurfaceFrame Frame;
        TArray<int32> Triangles;
        TArray<UE::Geometry::FGeneralPolygon2d> Regions;
        FBox Bounds = FBox(ForceInit);
        double AreaCm2 = 0;
    };

    struct FGeometry
    {
        FTriangleMesh Mesh;
        TUniquePtr<UE::Geometry::TMeshAABBTree3<FTriangleMesh>> Tree;
        TUniquePtr<UE::Geometry::TFastWindingTree<FTriangleMesh>> Winding;
        TArray<FPlanarSurface> Surfaces;
        TArray<int32> TriangleSurface;
        FBox Bounds = FBox(ForceInit);
        TArray<FString> Limitations;
        TArray<FVector3d> IslandSamples;
        TArray<int32> TriangleIsland;
        int32 InternalDuplicateTriangles = 0;
        int32 InternalIntersectionSegments = 0;
        int32 InternalCoplanarRegions = 0;
        TArray<FVector3d> InternalWitnesses;
        bool bClosedOriented = false;
        bool bPrepared = false;
    };

    struct FGeometryFact
    {
        FString Kind;
        int32 SurfaceA = INDEX_NONE;
        int32 SurfaceB = INDEX_NONE;
        FSurfaceFrame Frame;
        TArray<UE::Geometry::FGeneralPolygon2d> Regions;
        TArray<FVector3d> Witnesses;
        int32 TriangleHitCount = 0;
        bool bWitnessesTruncated = false;
        TOptional<double> AreaCm2;
        TOptional<double> SurfaceAreaACm2;
        TOptional<double> SurfaceAreaBCm2;
        double MinDistanceCm = 0;
        double MaxDistanceCm = 0;
        bool bSameFacing = false;
        FString Certainty = TEXT("source_geometry");
    };

    struct FGeometryPairResult
    {
        TArray<FGeometryFact> Facts;
        TArray<FString> Limitations;
        bool bComplete = false;
        bool bCancelled = false;
    };

    UEAGENTINTERFACE_API bool BuildGeometry(FGeometry& Geometry, const TFunction<bool()>& IsCancelled, FString& OutError);
    UEAGENTINTERFACE_API FGeometryPairResult CompareGeometry(
        const FGeometry& A, const FMatrix44d& TransformA,
        const FGeometry& B, const FMatrix44d& TransformB,
        double NearDistanceCm, const TFunction<bool()>& IsCancelled);
    UEAGENTINTERFACE_API bool UnionRegions(const TArray<UE::Geometry::FGeneralPolygon2d>& Input,
        TArray<UE::Geometry::FGeneralPolygon2d>& Output, double& OutAreaCm2);
}

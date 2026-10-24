#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "UeAgentModelingGridSolid.generated.h"

// Faces use outward mathematical winding, in grid-local centimeters.
USTRUCT()
struct FUeAgentGridSolidFace
{
	GENERATED_BODY()
	UPROPERTY() TArray<FVector> Vertices;
};

USTRUCT()
struct FUeAgentGridSolidPiece
{
	GENERATED_BODY()
	UPROPERTY() TArray<FUeAgentGridSolidFace> Faces;
};

struct FKAggregateGeom;
namespace UE::Geometry { class FDynamicMesh3; }

namespace UE::UAI::DomainAdapters::ModelingGeometry::GridSolid
{
	using FPiece = FUeAgentGridSolidPiece;
	using FFace = FUeAgentGridSolidFace;
	// Numerical tolerance in centimeters; it does not quantize to grid cells.
	constexpr double Tolerance = 1.e-5;
	FPiece MakeShape(const FString& Kind, const FVector& Size, const FTransform& ToGrid);
	FPiece MakeBox(const FVector& Min, const FVector& Max);
	double Volume(const FPiece& Piece);
	double Volume(const TArray<FPiece>& Pieces);
	FBox Bounds(const FPiece& Piece);
	bool Validate(const TArray<FPiece>& Pieces, UE::UAI::Contract::FDiagnostic& Error);
	bool Equivalent(const TArray<FPiece>& A, const TArray<FPiece>& B, UE::UAI::Contract::FDiagnostic* Error = nullptr);
	void Add(TArray<FPiece>& Pieces, const FPiece& Shape);
	void Remove(TArray<FPiece>& Pieces, const FPiece& Shape);
	bool BuildMesh(const TArray<FPiece>& Pieces, const FVector& Anchor,
		UE::Geometry::FDynamicMesh3& Mesh, int32& FaceCount, UE::UAI::Contract::FDiagnostic& Error);
	void BuildCollision(const TArray<FPiece>& Pieces, const FVector& Anchor, FKAggregateGeom& Collision);
	bool CollisionEquivalent(const FKAggregateGeom& Actual, const FKAggregateGeom& Expected, UE::UAI::Contract::FDiagnostic* Error = nullptr);
	bool MeshEquivalent(const UE::Geometry::FDynamicMesh3& Actual, const UE::Geometry::FDynamicMesh3& Expected, UE::UAI::Contract::FDiagnostic* Error = nullptr);
}

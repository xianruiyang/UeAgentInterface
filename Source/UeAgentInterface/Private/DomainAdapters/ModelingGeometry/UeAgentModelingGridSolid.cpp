#include "DomainAdapters/ModelingGeometry/UeAgentModelingGridSolid.h"

#include "Algo/Reverse.h"
#include "Dom/JsonValue.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMesh/DynamicMeshOverlay.h"
#include "DynamicMesh/Operations/MergeCoincidentMeshEdges.h"
#include "DynamicMeshEditor.h"
#include "PhysicsEngine/AggregateGeom.h"

namespace UE::UAI::DomainAdapters::ModelingGeometry::GridSolid
{
	namespace
	{
		bool DifferenceFact(UE::UAI::Contract::FDiagnostic* Error, const FString& Path, const FString& Actual, const FString& Expected)
		{
			if (Error)
			{
				*Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("voxel_grid_geometry_difference"),
					TEXT("The stored geometry differs from the intended geometry at this field."), TEXT("independent_readback"));
				Error->PropertyPath = Path; Error->ActualValue = MakeShared<FJsonValueString>(Actual);
				Error->ExpectedValue = MakeShared<FJsonValueString>(Expected);
			}
			return false;
		}
		FVector Normal(const FFace& Face)
		{
			FVector N = FVector::ZeroVector;
			for (int32 I = 1; I + 1 < Face.Vertices.Num(); ++I)
				N += FVector::CrossProduct(Face.Vertices[I] - Face.Vertices[0], Face.Vertices[I + 1] - Face.Vertices[0]);
			return N.GetSafeNormal(1.e-20);
		}
		void AddUnique(TArray<FVector>& Vertices, const FVector& V)
		{
			if (!Vertices.ContainsByPredicate([&](const FVector& P) { return P.Equals(V, Tolerance); })) Vertices.Add(V);
		}
		TArray<FVector> Vertices(const FPiece& Piece)
		{
			TArray<FVector> Result;
			for (const FFace& Face : Piece.Faces) for (const FVector& V : Face.Vertices) AddUnique(Result, V);
			return Result;
		}
		FFace ClipFace(const FFace& Face, const FVector& N, double D, TArray<FVector>* Cut = nullptr)
		{
			FFace Result;
			for (int32 I = 0; I < Face.Vertices.Num(); ++I)
			{
				const FVector A = Face.Vertices[I], B = Face.Vertices[(I + 1) % Face.Vertices.Num()];
				const double DA = FVector::DotProduct(N, A) - D, DB = FVector::DotProduct(N, B) - D;
				if (DA <= Tolerance) AddUnique(Result.Vertices, A);
				if ((DA < -Tolerance && DB > Tolerance) || (DA > Tolerance && DB < -Tolerance))
				{
					const FVector P = A + (B - A) * (DA / (DA - DB));
					AddUnique(Result.Vertices, P);
					if (Cut) AddUnique(*Cut, P);
				}
				if (Cut && FMath::Abs(DA) <= Tolerance) AddUnique(*Cut, A);
			}
			if (Result.Vertices.Num() < 3 || Normal(Result).IsNearlyZero()) Result.Vertices.Reset();
			return Result;
		}
		FPiece Clip(const FPiece& Piece, const FVector& N, double D)
		{
			const TArray<FVector> Points = Vertices(Piece);
			double Min = DBL_MAX, Max = -DBL_MAX;
			for (const FVector& V : Points) { const double Dist = FVector::DotProduct(N, V) - D; Min = FMath::Min(Min, Dist); Max = FMath::Max(Max, Dist); }
			if (Max <= Tolerance) return Piece;
			if (Min >= -Tolerance) return {};
			FPiece Result;
			TArray<FVector> Cap;
			for (const FFace& Face : Piece.Faces)
			{
				FFace Clipped = ClipFace(Face, N, D, &Cap);
				if (!Clipped.Vertices.IsEmpty()) Result.Faces.Add(MoveTemp(Clipped));
			}
			if (Cap.Num() >= 3)
			{
				FVector Center = FVector::ZeroVector;
				for (const FVector& V : Cap) Center += V;
				Center /= Cap.Num();
				FVector U, V;
				N.FindBestAxisVectors(U, V);
				Cap.Sort([&](const FVector& A, const FVector& B) {
					return FMath::Atan2(FVector::DotProduct(A - Center, V), FVector::DotProduct(A - Center, U))
						< FMath::Atan2(FVector::DotProduct(B - Center, V), FVector::DotProduct(B - Center, U)); });
				FFace Face; Face.Vertices = MoveTemp(Cap);
				if (FVector::DotProduct(Normal(Face), N) < 0) Algo::Reverse(Face.Vertices);
				Result.Faces.Add(MoveTemp(Face));
			}
			if (Volume(Result) <= Tolerance * Tolerance * Tolerance) return {};
			return Result;
		}
		bool VolumeOverlap(const FBox& A, const FBox& B)
		{
			return A.IsValid && B.IsValid && A.Max.X > B.Min.X + Tolerance && B.Max.X > A.Min.X + Tolerance
				&& A.Max.Y > B.Min.Y + Tolerance && B.Max.Y > A.Min.Y + Tolerance
				&& A.Max.Z > B.Min.Z + Tolerance && B.Max.Z > A.Min.Z + Tolerance;
		}
		void Difference(const FPiece& A, const FPiece& B, TArray<FPiece>& Result)
		{
			if (!VolumeOverlap(Bounds(A), Bounds(B))) { Result.Add(A); return; }
			FPiece Inside = A;
			for (const FFace& Face : B.Faces)
			{
				const FVector N = Normal(Face);
				const double D = FVector::DotProduct(N, Face.Vertices[0]);
				FPiece Outside = Clip(Inside, -N, -D);
				if (!Outside.Faces.IsEmpty()) Result.Add(MoveTemp(Outside));
				Inside = Clip(Inside, N, D);
				if (Inside.Faces.IsEmpty()) break;
			}
		}
		void SubtractFace(const FFace& Face, const FPiece& Cover, TArray<FFace>& Result)
		{
			FFace Inside = Face;
			for (const FFace& Boundary : Cover.Faces)
			{
				const FVector N = Normal(Boundary);
				const double D = FVector::DotProduct(N, Boundary.Vertices[0]);
				// The shared plane itself must not count as both inside and outside.
				double Max = -DBL_MAX, Min = DBL_MAX;
				for (const FVector& V : Inside.Vertices) { const double S = FVector::DotProduct(N, V) - D; Max = FMath::Max(Max, S); Min = FMath::Min(Min, S); }
				if (Max <= Tolerance) continue;
				if (Min >= -Tolerance) { Result.Add(MoveTemp(Inside)); return; }
				FFace Outside = ClipFace(Inside, -N, -D);
				if (!Outside.Vertices.IsEmpty()) Result.Add(MoveTemp(Outside));
				Inside = ClipFace(Inside, N, D);
				if (Inside.Vertices.IsEmpty()) return;
			}
		}
		FVector Quantized(const FVector& V)
		{
			return FVector(FMath::RoundToDouble(V.X / Tolerance) * Tolerance,
				FMath::RoundToDouble(V.Y / Tolerance) * Tolerance, FMath::RoundToDouble(V.Z / Tolerance) * Tolerance);
		}
	}

	FBox Bounds(const FPiece& Piece)
	{
		FBox Result(ForceInit);
		for (const FFace& Face : Piece.Faces) for (const FVector& V : Face.Vertices) Result += V;
		return Result;
	}
	double Volume(const FPiece& Piece)
	{
		const FBox Box = Bounds(Piece);
		if (!Box.IsValid) return 0;
		const FVector Center = Box.GetCenter();
		double Result = 0;
		for (const FFace& Face : Piece.Faces) for (int32 I = 1; I + 1 < Face.Vertices.Num(); ++I)
			Result += FVector::DotProduct(Face.Vertices[0] - Center,
				FVector::CrossProduct(Face.Vertices[I] - Center, Face.Vertices[I + 1] - Center)) / 6.0;
		return Result;
	}
	double Volume(const TArray<FPiece>& Pieces) { double V = 0; for (const FPiece& P : Pieces) V += Volume(P); return V; }

	FPiece MakeShape(const FString& Kind, const FVector& Size, const FTransform& ToGrid)
	{
		TArray<FVector> V;
		TArray<TArray<int32>> Indices;
		if (Kind == TEXT("square_pyramid"))
		{
			V = {{-.5,-.5,-.5},{.5,-.5,-.5},{.5,.5,-.5},{-.5,.5,-.5},{0,0,.5}};
			Indices = {{0,3,2,1},{0,1,4},{1,2,4},{2,3,4},{3,0,4}};
		}
		else if (Kind == TEXT("triangular_prism"))
		{
			// Right triangular XZ section: low at -X, high at +X; extrusion along Y.
			V = {{-.5,-.5,-.5},{.5,-.5,-.5},{.5,-.5,.5},{-.5,.5,-.5},{.5,.5,-.5},{.5,.5,.5}};
			Indices = {{0,1,2},{3,5,4},{0,3,4,1},{1,4,5,2},{2,5,3,0}};
		}
		else if (Kind == TEXT("box"))
		{
			V = {{-.5,-.5,-.5},{.5,-.5,-.5},{.5,.5,-.5},{-.5,.5,-.5},{-.5,-.5,.5},{.5,-.5,.5},{.5,.5,.5},{-.5,.5,.5}};
			Indices = {{0,3,2,1},{4,5,6,7},{0,1,5,4},{1,2,6,5},{2,3,7,6},{3,0,4,7}};
		}
		for (FVector& P : V) P = ToGrid.TransformPosition(P * Size);
		FPiece Result;
		for (const TArray<int32>& FaceIndices : Indices)
		{
			FFace Face; for (int32 I : FaceIndices) Face.Vertices.Add(V[I]); Result.Faces.Add(MoveTemp(Face));
		}
		return Result;
	}
	FPiece MakeBox(const FVector& Min, const FVector& Max) { return MakeShape(TEXT("box"), Max - Min, FTransform((Max + Min) * .5)); }

	void Add(TArray<FPiece>& Pieces, const FPiece& Shape)
	{
		TArray<FPiece> Pending{Shape};
		for (const FPiece& Existing : Pieces)
		{
			TArray<FPiece> Next;
			for (const FPiece& Part : Pending) Difference(Part, Existing, Next);
			Pending = MoveTemp(Next);
			if (Pending.IsEmpty()) return;
		}
		Pieces.Append(MoveTemp(Pending));
	}
	void Remove(TArray<FPiece>& Pieces, const FPiece& Shape)
	{
		TArray<FPiece> Result;
		for (const FPiece& Part : Pieces) Difference(Part, Shape, Result);
		Pieces = MoveTemp(Result);
	}

	bool Validate(const TArray<FPiece>& Pieces, UE::UAI::Contract::FDiagnostic& Error)
	{
		for (int32 I = 0; I < Pieces.Num(); ++I)
		{
			const FPiece& Piece = Pieces[I];
			bool bValid = Piece.Faces.Num() >= 4;
			const TArray<FVector> Points = Vertices(Piece);
			for (const FFace& Face : Piece.Faces)
			{
				bValid &= Face.Vertices.Num() >= 3 && !Normal(Face).IsNearlyZero();
				for (const FVector& V : Face.Vertices) bValid &= FMath::IsFinite(V.X) && FMath::IsFinite(V.Y) && FMath::IsFinite(V.Z);
				if (Face.Vertices.Num() < 3) continue;
				const FVector N = Normal(Face);
				const double D = FVector::DotProduct(N, Face.Vertices[0]);
				for (const FVector& V : Points) bValid &= FVector::DotProduct(N, V) <= D + Tolerance;
				for (const FVector& V : Face.Vertices) bValid &= FMath::Abs(FVector::DotProduct(N, V) - D) <= Tolerance;
				for (int32 E = 0; E < Face.Vertices.Num(); ++E)
				{
					const FVector& A = Face.Vertices[E];
					const FVector& B = Face.Vertices[(E + 1) % Face.Vertices.Num()];
					int32 ReverseEdges = 0;
					for (const FFace& Other : Piece.Faces) for (int32 K = 0; K < Other.Vertices.Num(); ++K)
						if (B.Equals(Other.Vertices[K], Tolerance) && A.Equals(Other.Vertices[(K + 1) % Other.Vertices.Num()], Tolerance)) ++ReverseEdges;
					bValid &= !A.Equals(B, Tolerance) && ReverseEdges == 1;
				}
			}
			const double V = Volume(Piece);
			if (!bValid || !FMath::IsFinite(V) || V <= Tolerance * Tolerance * Tolerance)
			{
				Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("voxel_grid_solid_invalid"),
					TEXT("The grid contains an invalid or zero-volume convex piece."), TEXT("solid_geometry"));
				Error.PropertyPath = FString::Printf(TEXT("/solid_pieces/%d"), I);
				Error.ActualValue = MakeShared<FJsonValueString>(FString::Printf(TEXT("faces=%d, volume_cm3=%.17g"), Piece.Faces.Num(), V));
				Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("finite closed convex piece with positive volume"));
				return false;
			}
		}
		return true;
	}
	bool Equivalent(const TArray<FPiece>& A, const TArray<FPiece>& B, UE::UAI::Contract::FDiagnostic* Error)
	{
		if (A.Num() != B.Num()) return DifferenceFact(Error, TEXT("/authoring_state/solid_pieces/count"), LexToString(A.Num()), LexToString(B.Num()));
		for (int32 I = 0; I < A.Num(); ++I)
		{
			const FString Path = FString::Printf(TEXT("/authoring_state/solid_pieces/%d/faces"), I);
			if (A[I].Faces.Num() != B[I].Faces.Num()) return DifferenceFact(Error, Path + TEXT("/count"), LexToString(A[I].Faces.Num()), LexToString(B[I].Faces.Num()));
			for (int32 J = 0; J < A[I].Faces.Num(); ++J)
			{
				const auto& AV = A[I].Faces[J].Vertices; const auto& BV = B[I].Faces[J].Vertices;
				const FString VPath = Path + FString::Printf(TEXT("/%d/vertices"), J);
				if (AV.Num() != BV.Num()) return DifferenceFact(Error, VPath + TEXT("/count"), LexToString(AV.Num()), LexToString(BV.Num()));
				for (int32 K = 0; K < AV.Num(); ++K) if (!AV[K].Equals(BV[K], Tolerance))
					return DifferenceFact(Error, VPath + FString::Printf(TEXT("/%d"), K), AV[K].ToString(), BV[K].ToString());
			}
		}
		return true;
	}

	bool BuildMesh(const TArray<FPiece>& Pieces, const FVector& Anchor, UE::Geometry::FDynamicMesh3& Mesh,
		int32& FaceCount, UE::UAI::Contract::FDiagnostic& Error)
	{
		using namespace UE::Geometry;
		if (!Validate(Pieces, Error)) return false;
		TArray<FFace> Faces;
		TArray<int32> FacePieces;
		for (int32 I = 0; I < Pieces.Num(); ++I) for (const FFace& Face : Pieces[I].Faces)
		{
			const FVector N = Normal(Face);
			const double D = FVector::DotProduct(N, Face.Vertices[0]);
			TArray<FFace> Visible{Face};
			for (int32 J = 0; J < Pieces.Num() && !Visible.IsEmpty(); ++J)
			{
				if (I == J) continue;
				const bool bAdjacent = Pieces[J].Faces.ContainsByPredicate([&](const FFace& Other) {
					return FVector::DotProduct(N, Normal(Other)) < -1.0 + 1.e-10
						&& FMath::Abs(FVector::DotProduct(N, Other.Vertices[0]) - D) <= Tolerance; });
				if (!bAdjacent) continue;
				TArray<FFace> Next;
				for (const FFace& Part : Visible) SubtractFace(Part, Pieces[J], Next);
				Visible = MoveTemp(Next);
			}
			for (FFace& Part : Visible)
			{
				Faces.Add(MoveTemp(Part)); FacePieces.Add(I);
			}
		}
		// Split polygon edges consistently, but weld only within each convex piece.
		// Separate solids may touch along an edge or point without sharing topology.
		TArray<FVector> Points;
		for (const FFace& Face : Faces) for (const FVector& P : Face.Vertices) AddUnique(Points, P);
		Mesh.Clear(); Mesh.EnableAttributes(); Mesh.Attributes()->EnableMaterialID();
		TArray<TMap<FVector, int32>> PieceVertexIds;
		PieceVertexIds.SetNum(Pieces.Num());
		FaceCount = Faces.Num();
		for (int32 FaceIndex = 0; FaceIndex < Faces.Num(); ++FaceIndex)
		{
			const FFace& Face = Faces[FaceIndex];
			TMap<FVector, int32>& VertexIds = PieceVertexIds[FacePieces[FaceIndex]];
			const auto Vertex = [&](const FVector& P) {
				const FVector Q = Quantized(P - Anchor);
				if (const int32* Id = VertexIds.Find(Q)) return *Id;
				const int32 Id = Mesh.AppendVertex(FVector3d(Q)); VertexIds.Add(Q, Id); return Id; };
			TArray<FVector> Boundary;
			for (int32 I = 0; I < Face.Vertices.Num(); ++I)
			{
				const FVector A = Face.Vertices[I], B = Face.Vertices[(I + 1) % Face.Vertices.Num()], Edge = B - A;
				const double Length2 = Edge.SizeSquared();
				if (Length2 <= Tolerance * Tolerance) continue;
				TArray<TPair<double, FVector>> Splits;
				Splits.Emplace(0, A);
				for (const FVector& P : Points)
				{
					const double T = FVector::DotProduct(P - A, Edge) / Length2;
					if (T > Tolerance / FMath::Sqrt(Length2) && T < 1 - Tolerance / FMath::Sqrt(Length2)
						&& (A + T * Edge).Equals(P, Tolerance)) Splits.Emplace(T, P);
				}
				Splits.Sort([](const auto& X, const auto& Y) { return X.Key < Y.Key; });
				for (const auto& Split : Splits) AddUnique(Boundary, Split.Value);
			}
			if (Boundary.Num() < 3) continue;
			FVector Center = FVector::ZeroVector;
			for (const FVector& P : Boundary) Center += P;
			Center /= Boundary.Num();
			const FVector N = Normal(Face);
			const int32 C = Vertex(Center);
			for (int32 I = 0; I < Boundary.Num(); ++I)
			{
				const FVector A = Boundary[I], B = Boundary[(I + 1) % Boundary.Num()];
				// Match UE's clockwise render winding; explicit normal points outside.
				const int32 T = Mesh.AppendTriangle(C, Vertex(B), Vertex(A));
				if (T < 0)
				{
					Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("voxel_grid_surface_invalid"),
						TEXT("The solid boundary could not be triangulated without a duplicate or non-manifold edge."), TEXT("solid_geometry"));
					Error.PropertyPath = TEXT("/mesh/triangles"); return false;
				}
				auto* Normals = Mesh.Attributes()->PrimaryNormals();
				Normals->SetTriangle(T, FIndex3i(Normals->AppendElement(FVector3f(N)), Normals->AppendElement(FVector3f(N)), Normals->AppendElement(FVector3f(N))));
				auto* UV = Mesh.Attributes()->PrimaryUV();
				const auto Uv = [&](const FVector& P) {
					const FVector Abs = N.GetAbs();
					return Abs.X >= Abs.Y && Abs.X >= Abs.Z ? FVector2f(P.Y * .01, 1 - P.Z * .01)
						: Abs.Y >= Abs.Z ? FVector2f(P.X * .01, 1 - P.Z * .01) : FVector2f(P.X * .01, 1 - P.Y * .01); };
				UV->SetTriangle(T, FIndex3i(UV->AppendElement(Uv(Center)), UV->AppendElement(Uv(B)), UV->AppendElement(Uv(A))));
				Mesh.Attributes()->GetMaterialID()->SetValue(T, 0);
			}
		}
		// Internal-face removal leaves matching boundary edges between pieces.
		// Native edge merging joins those seams without welding already-closed edges.
		FMergeCoincidentMeshEdges Merge(&Mesh);
		Merge.MergeVertexTolerance = Tolerance * 2;
		Merge.Apply();
		FDynamicMeshEditor Editor(&Mesh);
		FDynamicMeshEditResult SplitResult;
		Editor.SplitBowties(SplitResult);
		Mesh.CompactInPlace();
		const bool bValid = Mesh.CheckValidity(FDynamicMesh3::FValidityOptions(), EValidityCheckFailMode::ReturnOnly);
		// An empty subtraction result has no shell; UE IsClosed() reports false for it.
		const bool bClosed = Mesh.TriangleCount() == 0 || Mesh.IsClosed();
		if (!bValid || !bClosed)
		{
			int32 BoundaryEdges = 0;
			for (int32 Edge : Mesh.BoundaryEdgeIndicesItr()) ++BoundaryEdges;
			Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("voxel_grid_surface_invalid"),
				TEXT("The solid boundary did not form closed manifold shells after native seam joining."), TEXT("solid_geometry"));
			Error.PropertyPath = TEXT("/mesh/topology");
			Error.ActualValue = MakeShared<FJsonValueString>(FString::Printf(TEXT("valid=%s, closed=%s, boundary_edges=%d"), bValid ? TEXT("true") : TEXT("false"), bClosed ? TEXT("true") : TEXT("false"), BoundaryEdges));
			Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("valid=true, closed=true, boundary_edges=0"));
			return false;
		}
		return true;
	}

	void BuildCollision(const TArray<FPiece>& Pieces, const FVector& Anchor, FKAggregateGeom& Collision)
	{
		Collision = FKAggregateGeom();
		for (const FPiece& Piece : Pieces)
		{
			FKConvexElem Hull;
			Hull.VertexData = Vertices(Piece);
			for (const FFace& Face : Piece.Faces)
			{
				const auto Index = [&](const FVector& P) { return Hull.VertexData.IndexOfByPredicate([&](const FVector& V) { return V.Equals(P, Tolerance); }); };
				for (int32 I = 1; I + 1 < Face.Vertices.Num(); ++I)
				{
					Hull.IndexData.Add(Index(Face.Vertices[0])); Hull.IndexData.Add(Index(Face.Vertices[I])); Hull.IndexData.Add(Index(Face.Vertices[I + 1]));
				}
			}
			for (FVector& P : Hull.VertexData) P -= Anchor;
			Hull.UpdateElemBox();
			Collision.ConvexElems.Add(MoveTemp(Hull));
		}
	}
	bool CollisionEquivalent(const FKAggregateGeom& Actual, const FKAggregateGeom& Expected, UE::UAI::Contract::FDiagnostic* Error)
	{
		if (Actual.GetElementCount() != Expected.GetElementCount()) return DifferenceFact(Error, TEXT("/collision/shape_count"), LexToString(Actual.GetElementCount()), LexToString(Expected.GetElementCount()));
		if (Actual.ConvexElems.Num() != Expected.ConvexElems.Num()) return DifferenceFact(Error, TEXT("/collision/convex_count"), LexToString(Actual.ConvexElems.Num()), LexToString(Expected.ConvexElems.Num()));
		for (int32 I = 0; I < Actual.ConvexElems.Num(); ++I)
		{
			const FKConvexElem& A = Actual.ConvexElems[I]; const FKConvexElem& B = Expected.ConvexElems[I];
			const FString Path = FString::Printf(TEXT("/collision/convex_pieces/%d"), I);
			if (!A.GetTransform().Equals(B.GetTransform(), Tolerance)) return DifferenceFact(Error, Path + TEXT("/transform"), A.GetTransform().ToString(), B.GetTransform().ToString());
			if (A.VertexData.Num() != B.VertexData.Num()) return DifferenceFact(Error, Path + TEXT("/vertex_count"), LexToString(A.VertexData.Num()), LexToString(B.VertexData.Num()));
			for (const FVector& V : A.VertexData)
				if (!B.VertexData.ContainsByPredicate([&](const FVector& P) { return P.Equals(V, Tolerance); }))
					return DifferenceFact(Error, Path + TEXT("/unexpected_vertex"), V.ToString(), TEXT("vertex from the intended convex piece"));
		}
		return true;
	}
	bool MeshEquivalent(const UE::Geometry::FDynamicMesh3& Actual, const UE::Geometry::FDynamicMesh3& Expected, UE::UAI::Contract::FDiagnostic* Error)
	{
		if (Actual.VertexCount() != Expected.VertexCount()) return DifferenceFact(Error, TEXT("/mesh/vertex_count"), LexToString(Actual.VertexCount()), LexToString(Expected.VertexCount()));
		if (Actual.TriangleCount() != Expected.TriangleCount()) return DifferenceFact(Error, TEXT("/mesh/triangle_count"), LexToString(Actual.TriangleCount()), LexToString(Expected.TriangleCount()));
		for (int32 V : Expected.VertexIndicesItr()) if (!Actual.IsVertex(V) || !FVector(Actual.GetVertex(V)).Equals(FVector(Expected.GetVertex(V)), Tolerance))
			return DifferenceFact(Error, FString::Printf(TEXT("/mesh/vertices/%d"), V), Actual.IsVertex(V) ? FVector(Actual.GetVertex(V)).ToString() : TEXT("missing"), FVector(Expected.GetVertex(V)).ToString());
		for (int32 T : Expected.TriangleIndicesItr()) if (!Actual.IsTriangle(T) || Actual.GetTriangle(T) != Expected.GetTriangle(T))
		{
			const auto TriangleText = [](const UE::Geometry::FIndex3i& V) { return FString::Printf(TEXT("[%d,%d,%d]"), V.A, V.B, V.C); };
			return DifferenceFact(Error, FString::Printf(TEXT("/mesh/triangles/%d"), T), Actual.IsTriangle(T) ? TriangleText(Actual.GetTriangle(T)) : TEXT("missing"), TriangleText(Expected.GetTriangle(T)));
		}
		return true;
	}
}

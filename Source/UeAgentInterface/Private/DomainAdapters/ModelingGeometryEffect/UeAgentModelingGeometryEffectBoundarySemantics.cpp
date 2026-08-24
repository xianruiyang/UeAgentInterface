#include "DomainAdapters/ModelingGeometryEffect/UeAgentModelingGeometryEffectBoundarySemantics.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "Misc/SecureHash.h"

namespace UE::UAI::DomainAdapters::ModelingGeometryEffect
{
	using UE::Geometry::FDynamicMesh3;
	using UE::Geometry::FIndex2i;

	namespace
	{
		struct FBoundarySegment final
		{
			FVector3d A = FVector3d::Zero();
			FVector3d B = FVector3d::Zero();
		};

		bool PositionLess(const FVector3d& Left, const FVector3d& Right)
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
		}

		void HashUInt64(FSHA1& Hash, const uint64 Value)
		{
			uint8 Bytes[8];
			for (int32 Index = 0; Index < 8; ++Index)
			{
				Bytes[Index] = static_cast<uint8>(
					(Value >> (56 - Index * 8)) & 0xff);
			}
			Hash.Update(Bytes, UE_ARRAY_COUNT(Bytes));
		}

		void HashDouble(FSHA1& Hash, const double Value)
		{
			const double Normalized = Value == 0.0 ? 0.0 : Value;
			uint64 Bits = 0;
			FMemory::Memcpy(&Bits, &Normalized, sizeof(Bits));
			HashUInt64(Hash, Bits);
		}

		void HashPosition(FSHA1& Hash, const FVector3d& Position)
		{
			HashDouble(Hash, Position.X);
			HashDouble(Hash, Position.Y);
			HashDouble(Hash, Position.Z);
		}
	}

	bool FBoundarySemanticObservation::IsValid() const
	{
		return !Fingerprint.IsEmpty()
			&& EdgeCount >= 0
			&& VertexCount >= 0;
	}

	bool FBoundarySemanticObservation::operator==(
		const FBoundarySemanticObservation& Other) const
	{
		return Fingerprint == Other.Fingerprint
			&& EdgeCount == Other.EdgeCount
			&& VertexCount == Other.VertexCount;
	}

	bool FBoundarySemanticObservation::operator!=(
		const FBoundarySemanticObservation& Other) const
	{
		return !(*this == Other);
	}

	FBoundarySemanticObservation ObserveBoundarySemantics(
		const FDynamicMesh3& Mesh)
	{
		FBoundarySemanticObservation Result;
		TArray<FBoundarySegment> Segments;
		TSet<int32> BoundaryVertices;
		for (const int32 EdgeId : Mesh.EdgeIndicesItr())
		{
			if (!Mesh.IsBoundaryEdge(EdgeId))
			{
				continue;
			}
			const FIndex2i Edge = Mesh.GetEdgeV(EdgeId);
			FVector3d A = Mesh.GetVertex(Edge.A);
			FVector3d B = Mesh.GetVertex(Edge.B);
			if (PositionLess(B, A))
			{
				Swap(A, B);
			}
			Segments.Add({A, B});
			BoundaryVertices.Add(Edge.A);
			BoundaryVertices.Add(Edge.B);
		}
		Segments.Sort(
			[](const FBoundarySegment& Left, const FBoundarySegment& Right)
			{
				return Left.A != Right.A
					? PositionLess(Left.A, Right.A)
					: PositionLess(Left.B, Right.B);
			});

		FSHA1 Hash;
		const FTCHARToUTF8 Version(TEXT("uai-modeling-boundary-v1"));
		Hash.Update(
			reinterpret_cast<const uint8*>(Version.Get()),
			Version.Length());
		HashUInt64(Hash, static_cast<uint64>(Segments.Num()));
		for (const FBoundarySegment& Segment : Segments)
		{
			HashPosition(Hash, Segment.A);
			HashPosition(Hash, Segment.B);
		}
		Hash.Final();
		uint8 Digest[FSHA1::DigestSize];
		Hash.GetHash(Digest);

		Result.Fingerprint = BytesToHex(Digest, UE_ARRAY_COUNT(Digest));
		Result.EdgeCount = Segments.Num();
		Result.VertexCount = BoundaryVertices.Num();
		return Result;
	}
}

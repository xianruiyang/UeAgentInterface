// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentCustomModelingPrimitives.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "Generators/MeshShapeGenerator.h"
#include "InteractiveToolManager.h"
#include "Math/UnrealMathUtility.h"
#include "Math/Vector.h"

#define LOCTEXT_NAMESPACE "UeAgentCustomModelingPrimitives"

using namespace UE::Geometry;

namespace
{
	class FUeAgentSimplePrimitiveGenerator : public FMeshShapeGenerator
	{
	protected:
		int NextPolygonId = 0;
		EMakeMeshPolygroupMode PolygroupMode = EMakeMeshPolygroupMode::PerFace;

	public:
		void SetPolygroupMode(EMakeMeshPolygroupMode InPolygroupMode)
		{
			PolygroupMode = InPolygroupMode;
		}

	protected:
		int AddFacePolygon()
		{
			if (PolygroupMode == EMakeMeshPolygroupMode::PerShape)
			{
				return 0;
			}
			return NextPolygonId++;
		}

		void ComputeFaceAxes(
			const FVector3d& FaceNormal,
			const FVector3d& W0,
			const FVector3d& W1,
			FVector3d& OutUAxis,
			FVector3d& OutVAxis) const
		{
			const FVector3d Up = FVector3d::UnitZ();
			FVector3d ProjectedUp = Up - FaceNormal.Dot(Up) * FaceNormal;
			if (ProjectedUp.SquaredLength() > UE_DOUBLE_SMALL_NUMBER)
			{
				OutVAxis = ProjectedUp.GetSafeNormal();
				OutUAxis = OutVAxis.Cross(FaceNormal).GetSafeNormal();
				return;
			}

			FVector3d Edge = W1 - W0;
			Edge -= FaceNormal.Dot(Edge) * FaceNormal;
			if (Edge.SquaredLength() <= UE_DOUBLE_SMALL_NUMBER)
			{
				Edge = FVector3d::UnitX() - FaceNormal.Dot(FVector3d::UnitX()) * FaceNormal;
				if (Edge.SquaredLength() <= UE_DOUBLE_SMALL_NUMBER)
				{
					Edge = FVector3d::UnitY() - FaceNormal.Dot(FVector3d::UnitY()) * FaceNormal;
				}
			}

			OutUAxis = Edge.GetSafeNormal();
			OutVAxis = FaceNormal.Cross(OutUAxis).GetSafeNormal();
		}

		// FMeshShapeGenerator/UE primitive generators effectively expect clockwise winding
		// when viewed from the visible side. We therefore derive the outward normal from
		// a point known to be inside the solid, then flip triangle winding as needed so
		// the generated face is front-facing while the stored vertex normal still points out.
		void AppendTriFace(const FVector3d& A, const FVector3d& B, const FVector3d& C, const FVector3d& InsidePoint)
		{
			const int32 PolygonId = AddFacePolygon();
			FVector3d W0 = A;
			FVector3d W1 = B;
			FVector3d W2 = C;

			FVector3d Cross = (W1 - W0).Cross(W2 - W0);
			FVector3d FaceCenter = (W0 + W1 + W2) / 3.0;
			const bool bCrossPointsOutward = Cross.Dot(FaceCenter - InsidePoint) > 0.0;
			if (bCrossPointsOutward)
			{
				Swap(W1, W2);
				Cross = -Cross;
			}

			FVector3d OutwardNormal = -Cross;
			const double NormalLength = OutwardNormal.Length();
			OutwardNormal = (NormalLength > UE_DOUBLE_SMALL_NUMBER) ? (OutwardNormal / NormalLength) : FVector3d::UnitZ();
			const FVector3f Normal((float)OutwardNormal.X, (float)OutwardNormal.Y, (float)OutwardNormal.Z);

			const int32 VertA = AppendVertex(W0);
			const int32 VertB = AppendVertex(W1);
			const int32 VertC = AppendVertex(W2);

			FVector3d UAxis;
			FVector3d VAxis;
			ComputeFaceAxes(OutwardNormal, W0, W1, UAxis, VAxis);

			const double UvScale = 0.01;
			const double U0 = 0.0;
			const double V0 = 0.0;
			const double U1 = (W1 - W0).Dot(UAxis);
			const double V1 = (W1 - W0).Dot(VAxis);
			const double U2 = (W2 - W0).Dot(UAxis);
			const double V2 = (W2 - W0).Dot(VAxis);
			const double MinU = FMath::Min3(U0, U1, U2);
			const double MinV = FMath::Min3(V0, V1, V2);

			const int32 UvA = AppendUV(FVector2f((float)((U0 - MinU) * UvScale), (float)((V0 - MinV) * UvScale)), VertA);
			const int32 UvB = AppendUV(FVector2f((float)((U1 - MinU) * UvScale), (float)((V1 - MinV) * UvScale)), VertB);
			const int32 UvC = AppendUV(FVector2f((float)((U2 - MinU) * UvScale), (float)((V2 - MinV) * UvScale)), VertC);

			const int32 NormalA = AppendNormal(Normal, VertA);
			const int32 NormalB = AppendNormal(Normal, VertB);
			const int32 NormalC = AppendNormal(Normal, VertC);

			const int32 TriId = AppendTriangle(VertA, VertB, VertC);
			SetTriangleUVs(TriId, UvA, UvB, UvC);
			SetTriangleNormals(TriId, NormalA, NormalB, NormalC);
			SetTrianglePolygon(TriId, PolygonId);
		}

		void AppendQuadFace(const FVector3d& A, const FVector3d& B, const FVector3d& C, const FVector3d& D, const FVector3d& InsidePoint)
		{
			const int32 PolygonId = AddFacePolygon();
			FVector3d W0 = A;
			FVector3d W1 = B;
			FVector3d W2 = C;
			FVector3d W3 = D;

			FVector3d Cross = (W1 - W0).Cross(W2 - W0);
			FVector3d FaceCenter = (W0 + W1 + W2 + W3) / 4.0;
			const bool bCrossPointsOutward = Cross.Dot(FaceCenter - InsidePoint) > 0.0;
			if (bCrossPointsOutward)
			{
				Swap(W1, W3);
				Cross = -Cross;
			}

			FVector3d OutwardNormal = -Cross;
			const double NormalLength = OutwardNormal.Length();
			OutwardNormal = (NormalLength > UE_DOUBLE_SMALL_NUMBER) ? (OutwardNormal / NormalLength) : FVector3d::UnitZ();
			const FVector3f Normal((float)OutwardNormal.X, (float)OutwardNormal.Y, (float)OutwardNormal.Z);

			const int32 VertA = AppendVertex(W0);
			const int32 VertB = AppendVertex(W1);
			const int32 VertC = AppendVertex(W2);
			const int32 VertD = AppendVertex(W3);

			FVector3d UAxis;
			FVector3d VAxis;
			ComputeFaceAxes(OutwardNormal, W0, W1, UAxis, VAxis);

			const double UvScale = 0.01;
			const double U0 = 0.0;
			const double V0 = 0.0;
			const double U1 = (W1 - W0).Dot(UAxis);
			const double V1 = (W1 - W0).Dot(VAxis);
			const double U2 = (W2 - W0).Dot(UAxis);
			const double V2 = (W2 - W0).Dot(VAxis);
			const double U3 = (W3 - W0).Dot(UAxis);
			const double V3 = (W3 - W0).Dot(VAxis);
			const double MinU = FMath::Min(FMath::Min(U0, U1), FMath::Min(U2, U3));
			const double MinV = FMath::Min(FMath::Min(V0, V1), FMath::Min(V2, V3));

			const int32 UvA = AppendUV(FVector2f((float)((U0 - MinU) * UvScale), (float)((V0 - MinV) * UvScale)), VertA);
			const int32 UvB = AppendUV(FVector2f((float)((U1 - MinU) * UvScale), (float)((V1 - MinV) * UvScale)), VertB);
			const int32 UvC = AppendUV(FVector2f((float)((U2 - MinU) * UvScale), (float)((V2 - MinV) * UvScale)), VertC);
			const int32 UvD = AppendUV(FVector2f((float)((U3 - MinU) * UvScale), (float)((V3 - MinV) * UvScale)), VertD);

			const int32 NormalA = AppendNormal(Normal, VertA);
			const int32 NormalB = AppendNormal(Normal, VertB);
			const int32 NormalC = AppendNormal(Normal, VertC);
			const int32 NormalD = AppendNormal(Normal, VertD);

			const int32 Tri0 = AppendTriangle(VertA, VertB, VertC);
			SetTriangleUVs(Tri0, UvA, UvB, UvC);
			SetTriangleNormals(Tri0, NormalA, NormalB, NormalC);
			SetTrianglePolygon(Tri0, PolygonId);

			const int32 Tri1 = AppendTriangle(VertA, VertC, VertD);
			SetTriangleUVs(Tri1, UvA, UvC, UvD);
			SetTriangleNormals(Tri1, NormalA, NormalC, NormalD);
			SetTrianglePolygon(Tri1, PolygonId);
		}
	};

	class FUeAgentRampGenerator final : public FUeAgentSimplePrimitiveGenerator
	{
	public:
		double Depth = 100.0;
		double Width = 100.0;
		double Height = 100.0;

		virtual FMeshShapeGenerator& Generate() override
		{
			Reset();
			NextPolygonId = 0;

			const FVector3d A(0.0, 0.0, 0.0);
			const FVector3d B(0.0, Width, 0.0);
			const FVector3d C(Depth, 0.0, 0.0);
			const FVector3d D(Depth, Width, 0.0);
			const FVector3d E(Depth, 0.0, Height);
			const FVector3d F(Depth, Width, Height);
			const FVector3d InsidePoint(Depth * 0.75, Width * 0.5, Height * 0.25);

			AppendQuadFace(A, C, D, B, InsidePoint); // bottom
			AppendQuadFace(C, D, F, E, InsidePoint); // high end cap
			AppendQuadFace(A, B, F, E, InsidePoint); // slope
			AppendTriFace(A, E, C, InsidePoint);     // side at Y=0
			AppendTriFace(B, D, F, InsidePoint);     // side at Y=Width
			return *this;
		}
	};

	class FUeAgentRampCornerGenerator final : public FUeAgentSimplePrimitiveGenerator
	{
	public:
		double Depth = 100.0;
		double Width = 100.0;
		double Height = 100.0;

		virtual FMeshShapeGenerator& Generate() override
		{
			Reset();
			NextPolygonId = 0;

			const FVector3d A(0.0, 0.0, 0.0);
			const FVector3d B(Depth, 0.0, 0.0);
			const FVector3d C(Depth, Width, 0.0);
			const FVector3d D(0.0, Width, 0.0);
			const FVector3d P(Depth, Width, Height);
			const FVector3d InsidePoint((A + B + C + D + P) / 5.0);

			AppendQuadFace(A, B, C, D, InsidePoint); // bottom
			AppendTriFace(A, P, B, InsidePoint);     // front
			AppendTriFace(B, P, C, InsidePoint);     // right
			AppendTriFace(C, P, D, InsidePoint);     // back
			AppendTriFace(A, D, P, InsidePoint);     // left
			return *this;
		}
	};
}

bool UUeAgentAddPrimitiveToolBuilder::CanBuildTool(const FToolBuilderState& SceneState) const
{
	return true;
}

UInteractiveTool* UUeAgentAddPrimitiveToolBuilder::BuildTool(const FToolBuilderState& SceneState) const
{
	UAddPrimitiveTool* NewTool = nullptr;
	switch (ShapeType)
	{
	case EUeAgentPrimitiveShapeType::Ramp:
		NewTool = NewObject<UUeAgentAddRampPrimitiveTool>(SceneState.ToolManager);
		break;
	case EUeAgentPrimitiveShapeType::RampCorner:
		NewTool = NewObject<UUeAgentAddRampCornerPrimitiveTool>(SceneState.ToolManager);
		break;
	default:
		break;
	}

	if (NewTool)
	{
		NewTool->SetWorld(SceneState.World);
	}
	return NewTool;
}

UUeAgentAddPrimitiveToolBuilder* UUeAgentAddPrimitiveToolBuilder::CreateRampToolBuilder()
{
	UUeAgentAddPrimitiveToolBuilder* Builder = NewObject<UUeAgentAddPrimitiveToolBuilder>();
	Builder->ShapeType = EUeAgentPrimitiveShapeType::Ramp;
	return Builder;
}

UUeAgentAddPrimitiveToolBuilder* UUeAgentAddPrimitiveToolBuilder::CreateRampCornerToolBuilder()
{
	UUeAgentAddPrimitiveToolBuilder* Builder = NewObject<UUeAgentAddPrimitiveToolBuilder>();
	Builder->ShapeType = EUeAgentPrimitiveShapeType::RampCorner;
	return Builder;
}

UUeAgentAddRampPrimitiveTool::UUeAgentAddRampPrimitiveTool(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer.SetDefaultSubobjectClass<UUeAgentProceduralRampToolProperties>(TEXT("ShapeSettings")))
{
	AssetName = TEXT("Ramp");
	UInteractiveTool::SetToolDisplayName(LOCTEXT("RampToolName", "Create Ramp"));
}

void UUeAgentAddRampPrimitiveTool::GenerateMesh(FDynamicMesh3* OutMesh) const
{
	const UUeAgentProceduralRampToolProperties* RampSettings = Cast<UUeAgentProceduralRampToolProperties>(ShapeSettings);
	FUeAgentRampGenerator Generator;
	Generator.Depth = RampSettings ? RampSettings->Depth : 100.0;
	Generator.Width = RampSettings ? RampSettings->Width : 100.0;
	Generator.Height = RampSettings ? RampSettings->Height : 100.0;
	Generator.SetPolygroupMode(ShapeSettings ? ShapeSettings->PolygroupMode : EMakeMeshPolygroupMode::PerFace);
	Generator.Generate();
	OutMesh->Copy(&Generator);
}

UUeAgentAddRampCornerPrimitiveTool::UUeAgentAddRampCornerPrimitiveTool(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer.SetDefaultSubobjectClass<UUeAgentProceduralRampCornerToolProperties>(TEXT("ShapeSettings")))
{
	AssetName = TEXT("RampCorner");
	UInteractiveTool::SetToolDisplayName(LOCTEXT("RampCornerToolName", "Create Ramp Corner"));
}

void UUeAgentAddRampCornerPrimitiveTool::GenerateMesh(FDynamicMesh3* OutMesh) const
{
	const UUeAgentProceduralRampCornerToolProperties* CornerSettings = Cast<UUeAgentProceduralRampCornerToolProperties>(ShapeSettings);
	FUeAgentRampCornerGenerator Generator;
	Generator.Depth = CornerSettings ? CornerSettings->Depth : 100.0;
	Generator.Width = CornerSettings ? CornerSettings->Width : 100.0;
	Generator.Height = CornerSettings ? CornerSettings->Height : 100.0;
	Generator.SetPolygroupMode(ShapeSettings ? ShapeSettings->PolygroupMode : EMakeMeshPolygroupMode::PerFace);
	Generator.Generate();
	OutMesh->Copy(&Generator);
}

#undef LOCTEXT_NAMESPACE

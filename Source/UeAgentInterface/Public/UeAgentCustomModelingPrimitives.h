// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AddPrimitiveTool.h"

#include "UeAgentCustomModelingPrimitives.generated.h"

UENUM()
enum class EUeAgentPrimitiveShapeType : uint8
{
	Ramp,
	RampCorner
};

UCLASS()
class UEAGENTINTERFACE_API UUeAgentAddPrimitiveToolBuilder : public UInteractiveToolBuilder
{
	GENERATED_BODY()

public:
	virtual bool CanBuildTool(const FToolBuilderState& SceneState) const override;
	virtual UInteractiveTool* BuildTool(const FToolBuilderState& SceneState) const override;

	static UUeAgentAddPrimitiveToolBuilder* CreateRampToolBuilder();
	static UUeAgentAddPrimitiveToolBuilder* CreateRampCornerToolBuilder();

	EUeAgentPrimitiveShapeType ShapeType = EUeAgentPrimitiveShapeType::Ramp;
};

UCLASS()
class UEAGENTINTERFACE_API UUeAgentProceduralRampToolProperties : public UProceduralShapeToolProperties
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = Shape, meta = (UIMin = "1.0", UIMax = "1000.0", ClampMin = "0.0001", ClampMax = "1000000.0", ProceduralShapeSetting))
	float Width = 100.0f;

	UPROPERTY(EditAnywhere, Category = Shape, meta = (UIMin = "1.0", UIMax = "1000.0", ClampMin = "0.0001", ClampMax = "1000000.0", ProceduralShapeSetting))
	float Depth = 100.0f;

	UPROPERTY(EditAnywhere, Category = Shape, meta = (UIMin = "1.0", UIMax = "1000.0", ClampMin = "0.0001", ClampMax = "1000000.0", ProceduralShapeSetting))
	float Height = 100.0f;
};

UCLASS()
class UEAGENTINTERFACE_API UUeAgentProceduralRampCornerToolProperties : public UProceduralShapeToolProperties
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = Shape, meta = (UIMin = "1.0", UIMax = "1000.0", ClampMin = "0.0001", ClampMax = "1000000.0", ProceduralShapeSetting))
	float Width = 100.0f;

	UPROPERTY(EditAnywhere, Category = Shape, meta = (UIMin = "1.0", UIMax = "1000.0", ClampMin = "0.0001", ClampMax = "1000000.0", ProceduralShapeSetting))
	float Depth = 100.0f;

	UPROPERTY(EditAnywhere, Category = Shape, meta = (UIMin = "1.0", UIMax = "1000.0", ClampMin = "0.0001", ClampMax = "1000000.0", ProceduralShapeSetting))
	float Height = 100.0f;
};

UCLASS()
class UEAGENTINTERFACE_API UUeAgentAddRampPrimitiveTool : public UAddPrimitiveTool
{
	GENERATED_BODY()

public:
	explicit UUeAgentAddRampPrimitiveTool(const FObjectInitializer& ObjectInitializer);

protected:
	virtual void GenerateMesh(UE::Geometry::FDynamicMesh3* OutMesh) const override;
	virtual bool ShouldCenterXY() const override
	{
		return true;
	}
};

UCLASS()
class UEAGENTINTERFACE_API UUeAgentAddRampCornerPrimitiveTool : public UAddPrimitiveTool
{
	GENERATED_BODY()

public:
	explicit UUeAgentAddRampCornerPrimitiveTool(const FObjectInitializer& ObjectInitializer);

protected:
	virtual void GenerateMesh(UE::Geometry::FDynamicMesh3* OutMesh) const override;
	virtual bool ShouldCenterXY() const override
	{
		return true;
	}
};

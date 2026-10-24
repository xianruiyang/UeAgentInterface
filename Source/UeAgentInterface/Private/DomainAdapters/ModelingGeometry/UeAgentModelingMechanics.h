// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

#include "InteractiveToolManager.h"
#include "Selection/GeometrySelectionManager.h"
#include "Selection/GeometrySelector.h"
#include "VectorTypes.h"

class FEditorViewportClient;
class FJsonObject;
class FJsonValue;
class FProperty;
class UInteractiveTool;
class UModelingToolsEditorMode;
class UObject;
class UStaticMesh;
class UWorld;
class UEditorInteractiveToolsContext;
class UPrimitiveComponent;
class AActor;
struct FUeAgentRequestContext;

namespace UeAgentModelingOps
{
	using FGeometrySelectionUpdateConfig = UE::Geometry::FGeometrySelectionUpdateConfig;
	using FGeometrySelectionUpdateResult = UE::Geometry::FGeometrySelectionUpdateResult;
	using EGeometrySelectionChangeType = UE::Geometry::EGeometrySelectionChangeType;

	struct FModelingContext
	{
		FModelingContext();

		UModelingToolsEditorMode* Mode;
		UEditorInteractiveToolsContext* ToolsContext;
		FEditorViewportClient* ViewportClient;
	};

	struct FPrimitiveBoundsPlacement
	{
		FPrimitiveBoundsPlacement();

		bool bEnabled;
		FVector Center;
		FVector Extent;
		FRotator Rotation;
		FString PrimitiveKind;
	};

	enum class EModelingOutputTypePolicy : uint8
	{
		DynamicMeshActorDefault,
		DynamicMeshActorRequired,
		StaticMeshRequired
	};

	struct FModelingToolWrapperDefinition
	{
		FString ToolIdentifier;
		FString PostAction;
		FString UnsupportedActionError;
		bool bPostActionSupported = true;
		bool bPrimitiveCreation = false;
		EModelingOutputTypePolicy OutputTypePolicy = EModelingOutputTypePolicy::DynamicMeshActorDefault;
	};

	// Facts are constructed where the failing predicate is known. Input paths are
	// relative to the supplied parameter object; its caller binds the public path.
	UE::UAI::Contract::FDiagnostic ModelingFact(const FString& Code, const FString& Reason,
		const FString& Field, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
		const FString& Target = FString());
	UE::UAI::Contract::FDiagnostic ModelingCondition(const FString& Code, const FString& Reason,
		const FString& Field, bool Actual, bool Expected, const FString& Target = FString());
	UE::UAI::Contract::FDiagnostic ModelingInputFailure(const FString& Code, const FString& Reason,
		const TSharedPtr<FJsonObject>& Params, const FString& Field, TSharedPtr<FJsonValue> Expected);
	TSharedPtr<FJsonObject> MakeVectorJson(const FVector& Value);
	TSharedPtr<FJsonObject> MakeRotatorJson(const FRotator& Value);
	TSharedPtr<FJsonObject> MakeActorSummaryJson(AActor* Actor);
	bool TryGetNumberField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* FieldName, double& OutValue);
	bool TryGetVectorField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* FieldName, FVector& OutValue, UE::UAI::Contract::FDiagnostic& OutError);
	bool TryGetRotatorField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* FieldName, FRotator& OutValue, UE::UAI::Contract::FDiagnostic& OutError);
	void GetSelectedActors(TArray<AActor*>& OutActors);
	AActor* ResolveCreatedActorFromSelectionDelta(const TArray<AActor*>& BeforeSelection, const TArray<AActor*>& AfterSelection);
	bool PreparePrimitivePlacementParameters(
		const FString& WrapperCommand,
		const FString& ToolIdentifier,
		const TSharedPtr<FJsonObject>& Params,
		TSharedPtr<FJsonObject>& OutParams,
		FPrimitiveBoundsPlacement& OutPlacement,
		TSharedPtr<FJsonObject>& OutDerivedPrimitiveData,
		UE::UAI::Contract::FDiagnostic& OutError);
	bool ApplyPrimitivePlacementToActor(AActor* Actor, const FPrimitiveBoundsPlacement& Placement, UE::UAI::Contract::FDiagnostic& OutError);
	void ApplyOptionalCreatedActorMetadata(AActor* Actor, const TSharedPtr<FJsonObject>& Params);
	bool AutoGeneratePrimitiveCollisionIfMissing(
		AActor* Actor,
		const FPrimitiveBoundsPlacement& Placement,
		const TSharedPtr<FJsonObject>& DerivedPrimitiveData,
		const TSharedPtr<FJsonObject>& Params,
		TSharedPtr<FJsonObject>& OutData,
		UE::UAI::Contract::FDiagnostic& OutError);
	AActor* FindActorByNameOrLabelLoose(UWorld* World, const FString& NameOrLabel);
	UObject* LoadAssetObject(const FString& InPath);
	UStaticMesh* LoadStaticMeshAsset(const FString& InPath);
	UEAGENTINTERFACE_API const TMap<FString, FModelingToolWrapperDefinition>& GetModelingToolWrapperRegistry();
	UEAGENTINTERFACE_API bool ResolveAllowedModelingToolIdentifier(const FString& InValue, FString& OutToolIdentifier);
	FString ResolveToolIdentifierAlias(const FString& InValue);
	FEditorViewportClient* ResolveTickViewportClient(FEditorViewportClient* ViewportClient);
	void EnsureViewportClientFocused(FEditorViewportClient* ViewportClient);
	FString ElementTypeToString(UGeometrySelectionManager::EGeometryElementType ElementType);
	FString TopologyModeToString(UGeometrySelectionManager::EMeshTopologyMode Mode);
	UGeometrySelectionManager::EGeometryElementType ParseElementType(const FString& InValue);
	UGeometrySelectionManager::EMeshTopologyMode ParseTopologyMode(const FString& InValue);
	EGeometrySelectionChangeType ParseSelectionChangeType(const FString& InValue);
	FString SelectionChangeTypeToString(EGeometrySelectionChangeType InValue);
	bool ResolveViewportScreenPoint(const FEditorViewportClient* ViewportClient, const TSharedPtr<FJsonObject>& Params, FVector2D& OutScreenPoint, UE::UAI::Contract::FDiagnostic& OutError);
	bool DeprojectViewportScreenPoint(const FModelingContext& Context, const FVector2D& ScreenPoint, FRay3d& OutWorldRay, UE::UAI::Contract::FDiagnostic& OutError);
	bool ResolveWorldRayFromParams(const TSharedPtr<FJsonObject>& Params, FRay3d& OutWorldRay, UE::UAI::Contract::FDiagnostic& OutError);
	void FillSelectionUpdateJson(
		const UGeometrySelectionManager* SelectionManager,
		const FGeometrySelectionUpdateConfig& UpdateConfig,
		const FGeometrySelectionUpdateResult& Result,
		TSharedPtr<FJsonObject>& OutData);
	bool ImportResolvedProperty(UObject* Object, const FString& PropertyPath, const FString& ValueText, UE::UAI::Contract::FDiagnostic& OutError, FString* OutAppliedValueText, FString* OutCppType);
	void NotifyToolPropertySetUpdated(UInteractiveTool* Tool, UObject* PropertySet);
	UObject* FindToolPropertySet(UInteractiveTool* Tool, const FString& Selector);
	void AppendPropertySetJson(UObject* PropertySet, TArray<TSharedPtr<FJsonValue>>& OutPropertySets);
	bool GetModelingContext(FModelingContext& OutContext, UE::UAI::Contract::FDiagnostic& OutError);
	void FlushToolsContext(const FModelingContext& Context, int32 NumTicks);
	void FillActiveToolData(const FModelingContext& Context, TSharedPtr<FJsonObject>& OutData);
	bool StartToolByIdentifier(const FString& ToolIdentifier, const FModelingContext& Context, UE::UAI::Contract::FDiagnostic& OutError);
	void ConfigureDefaultModelingOutputType(EModelingOutputTypePolicy Policy);
	bool AutoPrepareModelingOutputType(const TSharedPtr<FJsonObject>& Params, UInteractiveTool* Tool, EModelingOutputTypePolicy Policy, UE::UAI::Contract::FDiagnostic& OutError, TSharedPtr<FJsonObject>& OutData);
	FString ResolveAcceptedOutputAssetPath(AActor* Actor);
	bool IsAcceptedDynamicMeshOutput(AActor* Actor);
	bool AutoPreparePrimitiveCreationTool(const TSharedPtr<FJsonObject>& Params, UInteractiveTool* Tool, const FString& ToolIdentifier, UE::UAI::Contract::FDiagnostic& OutError);
	bool FinishTool(const FModelingContext& Context, EToolShutdownType ShutdownType, UE::UAI::Contract::FDiagnostic& OutError);
	TArray<AActor*> ResolveActorsByIds(UWorld* World, const TArray<TSharedPtr<FJsonValue>>& IdValues);
	void SelectActors(const TArray<AActor*>& Actors);
	void SyncGeometryTargets(const TArray<AActor*>& Actors, const FModelingContext& Context);
	bool ApplyToolProperties(const TSharedPtr<FJsonObject>& Params, UInteractiveTool* Tool, UE::UAI::Contract::FDiagnostic& OutError, TArray<TSharedPtr<FJsonValue>>* OutPropertyResults);
	bool InvokeToolAction(UInteractiveTool* Tool, const FString& ActionName);
	bool ResolveSelectionActorsFromContext(const FUeAgentRequestContext& Ctx, UWorld* World, TArray<AActor*>& OutActors, UE::UAI::Contract::FDiagnostic& OutError);
	bool SaveActorStaticMeshes(const TArray<AActor*>& Actors, TArray<FString>& OutSavedPackagePaths, UE::UAI::Contract::FDiagnostic& OutError);
}

namespace UeAgentModelingSpatialInfo
{
	TSharedPtr<FJsonObject> MakeAabbJson(const FBox& BoundsBox);
	TSharedPtr<FJsonObject> MakePivotInAabbJson(const FBox& BoundsBox, TArray<TSharedPtr<FJsonValue>>& Issues);
	TSharedPtr<FJsonObject> MakeFrameJson(const FString& BoundsSpace);
	FString GetComponentTargetType(const UPrimitiveComponent* Component);
	bool ResolveActorFromParams(UWorld* World, const TSharedPtr<FJsonObject>& Params, AActor*& OutActor, UE::UAI::Contract::FDiagnostic& OutError);
	bool ResolveMeshComponent(AActor* Actor, const FString& ComponentName, UPrimitiveComponent*& OutComponent, UE::UAI::Contract::FDiagnostic& OutError);
	TSharedPtr<FJsonObject> MakeAssetTargetJson(const UObject* Asset, const FString& TargetType);
	TSharedPtr<FJsonObject> MakeComponentTargetJson(const AActor* Actor, const UPrimitiveComponent* Component, const FString& TargetType);
}

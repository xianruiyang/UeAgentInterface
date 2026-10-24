// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/ContentDiscovery/PaletteSearch/UeAgentBrushBuilderActorFactory.h"

#include "AssetRegistry/AssetData.h"
#include "Components/BrushComponent.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Brush.h"
#include "Engine/BrushBuilder.h"
#include "Engine/Level.h"
#include "Model.h"
#include "Engine/Polys.h"
#include "Engine/World.h"
#include "Materials/Material.h"
#include "Misc/ScopeExit.h"
#include "RenderingThread.h"

UUeAgentBrushBuilderActorFactory::UUeAgentBrushBuilderActorFactory()
{
	NewActorClass = ABrush::StaticClass();
	bShouldAutoRegister = false;
	bShowInEditorQuickMenu = false;
	bUseSurfaceOrientation = false;
	bUsePlacementExtent = false;
}

bool UUeAgentBrushBuilderActorFactory::CanCreateActorFrom(const FAssetData& AssetData, FText& OutErrorMsg)
{
	const UClass* BuilderClass = Cast<UClass>(AssetData.GetAsset());
	if (!BuilderClass || !BuilderClass->IsChildOf(UBrushBuilder::StaticClass())
		|| BuilderClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
	{
		OutErrorMsg = NSLOCTEXT("UeAgentInterface", "BrushBuilderClassRequired",
			"The placement item must publish a concrete native brush-builder class.");
		return false;
	}
	return true;
}

AActor* UUeAgentBrushBuilderActorFactory::SpawnActor(UObject* Asset, ULevel* Level,
	const FTransform& Transform, const FActorSpawnParameters& SpawnParameters)
{
	CreationDiagnostic.Reset();
	UClass* BuilderClass = Cast<UClass>(Asset);
	if (!GEditor || !Level || !BuilderClass || !BuilderClass->IsChildOf(UBrushBuilder::StaticClass()))
	{
		CreationDiagnostic = UE::UAI::Contract::FDiagnostic::Failure(
			TEXT("placeable_brush_creation_context_invalid"),
			TEXT("Native brush placement requires an Editor, target level and brush-builder class."), TEXT("candidate_apply"));
		return nullptr;
	}
	ABrush* Brush = Cast<ABrush>(Super::SpawnActor(Asset, Level, Transform, SpawnParameters));
	if (!Brush)
	{
		CreationDiagnostic = UE::UAI::Contract::FDiagnostic::Failure(
			TEXT("placeable_brush_actor_spawn_failed"), TEXT("Unreal could not create the native brush Actor."), TEXT("candidate_apply"));
		CreationDiagnostic.TargetPath = Level->GetPathName();
		return nullptr;
	}

	// Build directly into the new Actor. Never touch the editor's shared preview brush.
	const EObjectFlags Flags = Brush->GetFlags() & (RF_Transactional | RF_Transient);
	Brush->BrushType = Brush_Add;
	Brush->PolyFlags = 0;
	Brush->SetNotForClientOrServer();
	Brush->Brush = NewObject<UModel>(Brush, NAME_None, Flags);
	Brush->Brush->Initialize(nullptr, true);
	Brush->Brush->Polys = NewObject<UPolys>(Brush->Brush, NAME_None, Flags);
	Brush->GetBrushComponent()->Brush = Brush->Brush;
	Brush->BrushBuilder = NewObject<UBrushBuilder>(Brush, BuilderClass, NAME_None, Flags);
	const FVector OriginalPivot = GEditor->GetPivotLocation();
	ON_SCOPE_EXIT { GEditor->SetPivot(OriginalPivot, false, true); };
	const bool bBuilt = Brush->BrushBuilder->Build(Level->OwningWorld, Brush);
	const int32 PolygonCount = Brush->Brush->Polys->Element.Num();
	if (!bBuilt || PolygonCount == 0)
	{
		CreationDiagnostic = UE::UAI::Contract::FDiagnostic::Failure(
			TEXT("placeable_brush_geometry_build_failed"),
			bBuilt ? TEXT("The native brush builder produced no polygons.") : TEXT("The native brush builder rejected geometry creation."),
			TEXT("candidate_apply"));
		CreationDiagnostic.PropertyPath = TEXT("brush.polygons");
		CreationDiagnostic.TargetPath = BuilderClass->GetPathName();
		CreationDiagnostic.ActualValue = MakeShared<FJsonValueNumber>(PolygonCount);
		CreationDiagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-empty native brush geometry"));
		Brush->Destroy();
		return nullptr;
	}
	for (FPoly& Polygon : Brush->Brush->Polys->Element)
	{
		if (!Polygon.Material) Polygon.Material = UMaterial::GetDefaultMaterial(MD_Surface);
	}
	// Native builders may snap their work brush; the public placement transform is authoritative.
	Brush->SetActorTransform(Transform);
	Brush->Brush->BuildBound();
	Brush->ReregisterAllComponents();
	return Brush;
}

bool UUeAgentBrushBuilderActorFactory::SettleLevel(ULevel& Level,
	UE::UAI::Contract::FDiagnostic& OutError)
{
	OutError.Reset();
	UWorld* World = Level.OwningWorld;
	if (!GEditor || !World || !Level.Model)
	{
		OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("placeable_brush_level_unavailable"),
			TEXT("The native BSP rebuild requires an Editor, owning world and level model."), TEXT("native_settle"));
		OutError.TargetPath = Level.GetPathName();
		return false;
	}
	// Explicit authoring must settle even when interactive BSP auto-update is disabled.
	// Use the native level-scoped primitives, without changing the current level or selection.
	World->InvalidateModelGeometry(&Level);
	FlushRenderingCommands();
	GEditor->RebuildModelFromBrushes(Level.Model, false);
	Level.UpdateModelComponents();
	GEditor->RebuildStaticNavigableGeometry(&Level);
	Level.MarkPackageDirty();
	return true;
}

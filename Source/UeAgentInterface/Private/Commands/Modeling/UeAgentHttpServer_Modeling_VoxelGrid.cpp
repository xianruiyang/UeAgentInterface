// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_Modeling.h"
#include "UeAgentHttpServer_ModelingOps.h"

#include "Module/UeAgentInterfaceLogger.h"
#include "Utilities/UeAgentJsonDiagnostics.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/BoxComponent.h"
#include "Components/DynamicMeshComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMeshActor.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "PhysicsEngine/BodySetup.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UDynamicMesh.h"

namespace UeAgentModelingVoxelGrid
{
	static constexpr const TCHAR* ActorTagPrefix = TEXT("UAIEditableVoxelGrid:");
	static constexpr const TCHAR* CollisionTagPrefix = TEXT("UAIEditableVoxelGridCollision:");
	static constexpr const TCHAR* LegacyCheckerVoxelGridMaterialPath = TEXT("/MeshModelingToolsetExp/Materials/CheckerMaterial");
	static constexpr const TCHAR* LegacyWorkingVoxelGridMaterialPath = TEXT("/MeshModelingToolsetExp/Materials/InProgressMaterial");
	static constexpr const TCHAR* LegacySimpleBaseVoxelGridMaterialPath = TEXT("/MeshModelingToolsetExp/Materials/SimpleBaseMaterial");
	static constexpr int64 DefaultMaxOperationCells = 200000;
	static constexpr int32 DefaultMaxCollisionBoxes = 2048;

	struct FCellRange
	{
		FIntVector Min = FIntVector::ZeroValue;
		FIntVector MaxExclusive = FIntVector::ZeroValue;
	};

	FVoxelGridSession::FVoxelGridSession()
		: CellSize(100.0)
		, GridToWorld(FTransform::Identity)
		, AnchorLocal(FVector::ZeroVector)
		, Revision(0)
		, bDirty(false)
	{
	}

	struct FCollisionBoxRange
	{
		FIntVector Min = FIntVector::ZeroValue;
		FIntVector MaxExclusive = FIntVector::ZeroValue;
	};

	TMap<FString, FVoxelGridSession> GVoxelGridSessions;

	static FString MakeGuidSuffix()
	{
		return FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12);
	}

	FString MakeNewSessionId()
	{
		return FString::Printf(TEXT("VG_SESSION_%s"), *MakeGuidSuffix());
	}

	static FString MakeNewGridId()
	{
		return FString::Printf(TEXT("VG_%s"), *MakeGuidSuffix());
	}

	static FName MakeUniqueVoxelGridActorName(UWorld* World, const FString& GridId)
	{
		const FString BaseName = FString::Printf(TEXT("UAI_VoxelGrid_%s"), *GridId);
		UObject* ActorOuter = World ? Cast<UObject>(World->PersistentLevel) : nullptr;
		if (!ActorOuter)
		{
			ActorOuter = World;
		}
		return MakeUniqueObjectName(ActorOuter, ADynamicMeshActor::StaticClass(), FName(*BaseName));
	}

	static FString MakeActorTag(const FString& GridId)
	{
		return FString(ActorTagPrefix) + GridId;
	}

	static FString MakeCollisionTag(const FString& GridId)
	{
		return FString(CollisionTagPrefix) + GridId;
	}

	FString GetVoxelGridSidecarPath(const FString& GridId)
	{
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAgentInterface"), TEXT("VoxelGrids"), GridId + TEXT(".json"));
	}

	FVector GridToComponentLocal(const FVoxelGridSession& Session, const FVector& GridLocalPoint)
	{
		return GridLocalPoint - Session.AnchorLocal;
	}

	FVector ComponentLocalToGrid(const FVoxelGridSession& Session, const FVector& ComponentLocalPoint)
	{
		return ComponentLocalPoint + Session.AnchorLocal;
	}

	FTransform MakeActorTransformFromGridAndAnchor(const FVoxelGridSession& Session)
	{
		FTransform ActorTransform = Session.GridToWorld;
		ActorTransform.SetLocation(Session.GridToWorld.TransformPosition(Session.AnchorLocal));
		ActorTransform.SetScale3D(FVector::OneVector);
		return ActorTransform;
	}

	static TArray<FIntVector> GetSortedCells(const TSet<FIntVector>& Cells)
	{
		TArray<FIntVector> Sorted;
		Sorted.Reserve(Cells.Num());
		for (const FIntVector& Cell : Cells)
		{
			Sorted.Add(Cell);
		}
		Sorted.Sort([](const FIntVector& A, const FIntVector& B)
		{
			if (A.Z != B.Z)
			{
				return A.Z < B.Z;
			}
			if (A.Y != B.Y)
			{
				return A.Y < B.Y;
			}
			return A.X < B.X;
		});
		return Sorted;
	}

	static TSharedPtr<FJsonObject> MakeIntVectorJson(const FIntVector& Value)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("x"), Value.X);
		Obj->SetNumberField(TEXT("y"), Value.Y);
		Obj->SetNumberField(TEXT("z"), Value.Z);
		return Obj;
	}

	static TSharedPtr<FJsonObject> MakeRangeJson(const FCellRange& Range)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetObjectField(TEXT("min"), MakeIntVectorJson(Range.Min));
		Obj->SetObjectField(TEXT("max_exclusive"), MakeIntVectorJson(Range.MaxExclusive));
		return Obj;
	}

	static TSharedPtr<FJsonObject> MakeBoxRangeJson(const FCollisionBoxRange& Range)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetObjectField(TEXT("min"), MakeIntVectorJson(Range.Min));
		Obj->SetObjectField(TEXT("max_exclusive"), MakeIntVectorJson(Range.MaxExclusive));
		return Obj;
	}

	static TSharedPtr<FJsonObject> MakeFBoxJson(const FBox& Box)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("valid"), Box.IsValid != 0);
		if (Box.IsValid != 0)
		{
			Obj->SetObjectField(TEXT("min"), UeAgentModelingOps::MakeVectorJson(Box.Min));
			Obj->SetObjectField(TEXT("max"), UeAgentModelingOps::MakeVectorJson(Box.Max));
			Obj->SetObjectField(TEXT("center"), UeAgentModelingOps::MakeVectorJson(Box.GetCenter()));
			Obj->SetObjectField(TEXT("extent"), UeAgentModelingOps::MakeVectorJson(Box.GetExtent()));
		}
		return Obj;
	}

	static bool TryGetObjectField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* FieldName, TSharedPtr<FJsonObject>& OutObject)
	{
		const TSharedPtr<FJsonObject>* ObjectPtr = nullptr;
		if (!Obj.IsValid() || !Obj->TryGetObjectField(FieldName, ObjectPtr) || !ObjectPtr || !ObjectPtr->IsValid())
		{
			return false;
		}
		OutObject = *ObjectPtr;
		return true;
	}

	static FString GetOptionalStringField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* FieldName, const FString& DefaultValue)
	{
		FString Value;
		if (Obj.IsValid() && Obj->TryGetStringField(FieldName, Value))
		{
			return Value.TrimStartAndEnd();
		}
		return DefaultValue;
	}

	static bool TryGetNestedStringField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* ObjectName, const TCHAR* FieldName, FString& OutValue)
	{
		TSharedPtr<FJsonObject> Nested;
		if (!TryGetObjectField(Obj, ObjectName, Nested))
		{
			return false;
		}
		return Nested->TryGetStringField(FieldName, OutValue);
	}

	static bool TryGetOptionalInt64Field(const TSharedPtr<FJsonObject>& Obj, const TCHAR* FieldName, int64& OutValue)
	{
		double Number = 0.0;
		if (!Obj.IsValid() || !Obj->TryGetNumberField(FieldName, Number))
		{
			return false;
		}
		OutValue = static_cast<int64>(FMath::RoundToDouble(Number));
		return true;
	}

	static void NormalizeBounds(FVector& Min, FVector& Max)
	{
		if (Min.X > Max.X)
		{
			Swap(Min.X, Max.X);
		}
		if (Min.Y > Max.Y)
		{
			Swap(Min.Y, Max.Y);
		}
		if (Min.Z > Max.Z)
		{
			Swap(Min.Z, Max.Z);
		}
	}

	FBox TransformBoxBy(const FBox& Box, const FTransform& Transform)
	{
		FBox Result(ForceInit);
		if (Box.IsValid == 0)
		{
			return Result;
		}

		for (int32 XIndex = 0; XIndex < 2; ++XIndex)
		{
			for (int32 YIndex = 0; YIndex < 2; ++YIndex)
			{
				for (int32 ZIndex = 0; ZIndex < 2; ++ZIndex)
				{
					const FVector Corner(
						XIndex == 0 ? Box.Min.X : Box.Max.X,
						YIndex == 0 ? Box.Min.Y : Box.Max.Y,
						ZIndex == 0 ? Box.Min.Z : Box.Max.Z);
					Result += Transform.TransformPosition(Corner);
				}
			}
		}
		return Result;
	}

	static int64 GetRangeCellCount(const FCellRange& Range)
	{
		return static_cast<int64>(Range.MaxExclusive.X - Range.Min.X) *
			static_cast<int64>(Range.MaxExclusive.Y - Range.Min.Y) *
			static_cast<int64>(Range.MaxExclusive.Z - Range.Min.Z);
	}

	static bool ValidateRange(const FCellRange& Range)
	{
		return Range.MaxExclusive.X > Range.Min.X &&
			Range.MaxExclusive.Y > Range.Min.Y &&
			Range.MaxExclusive.Z > Range.Min.Z;
	}

	bool TryGetGridIdFromActor(AActor* Actor, FString& OutGridId)
	{
		if (!Actor)
		{
			return false;
		}

		for (const FName& TagName : Actor->Tags)
		{
			const FString Tag = TagName.ToString();
			if (Tag.StartsWith(ActorTagPrefix))
			{
				OutGridId = Tag.RightChop(FCString::Strlen(ActorTagPrefix));
				return !OutGridId.IsEmpty();
			}
		}
		return false;
	}

	static void SetActorGridTag(AActor* Actor, const FString& GridId)
	{
		if (!Actor)
		{
			return;
		}

		Actor->Tags.RemoveAll([](const FName& TagName)
		{
			return TagName.ToString().StartsWith(ActorTagPrefix);
		});
		Actor->Tags.AddUnique(FName(*MakeActorTag(GridId)));
	}

	static UDynamicMeshComponent* FindDynamicMeshComponent(AActor* Actor)
	{
		if (!Actor)
		{
			return nullptr;
		}

		if (ADynamicMeshActor* DynamicMeshActor = Cast<ADynamicMeshActor>(Actor))
		{
			return DynamicMeshActor->GetDynamicMeshComponent();
		}

		TArray<UActorComponent*> Components;
		Actor->GetComponents(UDynamicMeshComponent::StaticClass(), Components);
		for (UActorComponent* Component : Components)
		{
			if (UDynamicMeshComponent* DynamicMeshComponent = Cast<UDynamicMeshComponent>(Component))
			{
				return DynamicMeshComponent;
			}
		}
		return nullptr;
	}

	static void RemoveVoxelCollisionComponents(AActor* Actor, const FString& GridId)
	{
		if (!Actor)
		{
			return;
		}

		const FString ExpectedTag = MakeCollisionTag(GridId);
		TArray<UActorComponent*> Components;
		Actor->GetComponents(UBoxComponent::StaticClass(), Components);
		for (UActorComponent* Component : Components)
		{
			if (!Component)
			{
				continue;
			}

			bool bMatches = false;
			for (const FName& TagName : Component->ComponentTags)
			{
				const FString Tag = TagName.ToString();
				if ((!GridId.IsEmpty() && Tag == ExpectedTag) || (GridId.IsEmpty() && Tag.StartsWith(CollisionTagPrefix)))
				{
					bMatches = true;
					break;
				}
			}

			if (bMatches)
			{
				Component->Modify();
				Component->DestroyComponent();
			}
		}
	}

	static bool TryResolveVoxelActor(UWorld* World, const TSharedPtr<FJsonObject>& Params, AActor*& OutActor, FString& OutError)
	{
		FString ActorId;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("actor_id"), ActorId) || ActorId.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("actor_id_required_for_edit_mode");
			return false;
		}

		OutActor = UeAgentModelingOps::FindActorByNameOrLabelLoose(World, ActorId.TrimStartAndEnd());
		if (!OutActor)
		{
			OutError = TEXT("voxel_grid_actor_not_found");
			return false;
		}

		if (!OutActor->GetActorScale3D().Equals(FVector::OneVector, 0.001))
		{
			OutError = TEXT("voxel_grid_actor_scale_not_supported");
			return false;
		}

		if (!FindDynamicMeshComponent(OutActor))
		{
			OutError = TEXT("voxel_grid_dynamic_mesh_component_required");
			return false;
		}

		return true;
	}

	static bool TryParseGridParams(const TSharedPtr<FJsonObject>& Params, double& OutCellSize, FVector& OutOrigin, FRotator& OutRotation, FString& OutError)
	{
		using namespace UeAgentModelingOps;

		TSharedPtr<FJsonObject> GridObj = Params;
		TSharedPtr<FJsonObject> NestedGrid;
		if (TryGetObjectField(Params, TEXT("grid"), NestedGrid))
		{
			GridObj = NestedGrid;
		}

		OutCellSize = 100.0;
		double CellSizeValue = 0.0;
		if (TryGetNumberField(GridObj, TEXT("cell_size"), CellSizeValue) || TryGetNumberField(Params, TEXT("cell_size"), CellSizeValue))
		{
			OutCellSize = CellSizeValue;
		}

		if (OutCellSize <= KINDA_SMALL_NUMBER)
		{
			OutError = TEXT("cell_size_must_be_positive");
			return false;
		}

		OutOrigin = FVector::ZeroVector;
		if (!TryGetVectorField(GridObj, TEXT("origin"), OutOrigin))
		{
			TryGetVectorField(Params, TEXT("origin"), OutOrigin);
		}

		OutRotation = FRotator::ZeroRotator;
		if (!TryGetRotatorField(GridObj, TEXT("rotation"), OutRotation))
		{
			TryGetRotatorField(Params, TEXT("rotation"), OutRotation);
		}

		return true;
	}

	static bool TryParseBoundsRange(const TSharedPtr<FJsonObject>& Params, const FTransform& GridToWorld, const FVector& AnchorLocal, double CellSize, FCellRange& OutRange, TSharedPtr<FJsonObject>& OutReadback, FString& OutError)
	{
		using namespace UeAgentModelingOps;

		TSharedPtr<FJsonObject> BoundsObj = Params;
		TSharedPtr<FJsonObject> NestedBounds;
		if (TryGetObjectField(Params, TEXT("bounds"), NestedBounds))
		{
			BoundsObj = NestedBounds;
		}

		FVector BoundsMin = FVector::ZeroVector;
		FVector BoundsMax = FVector::ZeroVector;
		FVector BoundsCenter = FVector::ZeroVector;
		FVector BoundsExtent = FVector::ZeroVector;

		bool bHasMinMax = TryGetVectorField(BoundsObj, TEXT("min"), BoundsMin) && TryGetVectorField(BoundsObj, TEXT("max"), BoundsMax);
		if (!bHasMinMax)
		{
			bHasMinMax = TryGetVectorField(Params, TEXT("bounds_min"), BoundsMin) && TryGetVectorField(Params, TEXT("bounds_max"), BoundsMax);
		}

		if (!bHasMinMax)
		{
			const bool bHasCenterExtent =
				(TryGetVectorField(BoundsObj, TEXT("center"), BoundsCenter) && TryGetVectorField(BoundsObj, TEXT("extent"), BoundsExtent)) ||
				(TryGetVectorField(Params, TEXT("bounds_center"), BoundsCenter) && TryGetVectorField(Params, TEXT("bounds_extent"), BoundsExtent));
			if (!bHasCenterExtent)
			{
				OutError = TEXT("bounds_min_max_or_center_extent_required");
				return false;
			}
			BoundsMin = BoundsCenter - BoundsExtent;
			BoundsMax = BoundsCenter + BoundsExtent;
		}

		NormalizeBounds(BoundsMin, BoundsMax);
		if ((BoundsMax - BoundsMin).GetMin() <= KINDA_SMALL_NUMBER)
		{
			OutError = TEXT("bounds_empty");
			return false;
		}

		FString BoundsSpace = TEXT("world");
		FString SpaceValue;
		if ((BoundsObj.IsValid() && BoundsObj->TryGetStringField(TEXT("space"), SpaceValue)) ||
			(Params.IsValid() && Params->TryGetStringField(TEXT("bounds_space"), SpaceValue)))
		{
			BoundsSpace = SpaceValue.TrimStartAndEnd().ToLower();
		}

		FVector LocalMin = BoundsMin;
		FVector LocalMax = BoundsMax;
		if (BoundsSpace == TEXT("world"))
		{
			FBox LocalBox(ForceInit);
			for (int32 XIndex = 0; XIndex < 2; ++XIndex)
			{
				for (int32 YIndex = 0; YIndex < 2; ++YIndex)
				{
					for (int32 ZIndex = 0; ZIndex < 2; ++ZIndex)
					{
						const FVector Corner(
							XIndex == 0 ? BoundsMin.X : BoundsMax.X,
							YIndex == 0 ? BoundsMin.Y : BoundsMax.Y,
							ZIndex == 0 ? BoundsMin.Z : BoundsMax.Z);
						LocalBox += GridToWorld.InverseTransformPosition(Corner);
					}
				}
			}

			if (LocalBox.IsValid == 0)
			{
				OutError = TEXT("bounds_world_to_grid_failed");
				return false;
			}
			LocalMin = LocalBox.Min;
			LocalMax = LocalBox.Max;
		}
		else if (BoundsSpace == TEXT("grid") || BoundsSpace == TEXT("grid_local") || BoundsSpace == TEXT("voxel_grid"))
		{
			NormalizeBounds(LocalMin, LocalMax);
		}
		else if (BoundsSpace == TEXT("local") || BoundsSpace == TEXT("actor") || BoundsSpace == TEXT("actor_local") || BoundsSpace == TEXT("component") || BoundsSpace == TEXT("component_local"))
		{
			LocalMin += AnchorLocal;
			LocalMax += AnchorLocal;
			NormalizeBounds(LocalMin, LocalMax);
		}
		else
		{
			OutError = TEXT("unsupported_bounds_space");
			return false;
		}

		OutRange.Min.X = FMath::FloorToInt(LocalMin.X / CellSize);
		OutRange.Min.Y = FMath::FloorToInt(LocalMin.Y / CellSize);
		OutRange.Min.Z = FMath::FloorToInt(LocalMin.Z / CellSize);
		OutRange.MaxExclusive.X = FMath::CeilToInt(LocalMax.X / CellSize);
		OutRange.MaxExclusive.Y = FMath::CeilToInt(LocalMax.Y / CellSize);
		OutRange.MaxExclusive.Z = FMath::CeilToInt(LocalMax.Z / CellSize);

		if (!ValidateRange(OutRange))
		{
			OutError = TEXT("bounds_do_not_cover_any_voxel_cells");
			return false;
		}

		int64 MaxCells = DefaultMaxOperationCells;
		TryGetOptionalInt64Field(Params, TEXT("max_operation_cells"), MaxCells);
		const int64 CellCount = GetRangeCellCount(OutRange);
		if (CellCount > MaxCells)
		{
			OutError = TEXT("voxel_operation_cell_limit_exceeded");
			return false;
		}

		OutReadback = MakeShared<FJsonObject>();
		OutReadback->SetStringField(TEXT("input_space"), BoundsSpace);
		OutReadback->SetObjectField(TEXT("input_min"), MakeVectorJson(BoundsMin));
		OutReadback->SetObjectField(TEXT("input_max"), MakeVectorJson(BoundsMax));
		OutReadback->SetObjectField(TEXT("grid_local_min"), MakeVectorJson(LocalMin));
		OutReadback->SetObjectField(TEXT("grid_local_max"), MakeVectorJson(LocalMax));
		OutReadback->SetObjectField(TEXT("cell_range"), MakeRangeJson(OutRange));
		OutReadback->SetNumberField(TEXT("cell_count"), CellCount);
		return true;
	}

	static bool CheckExpectedRevision(const TSharedPtr<FJsonObject>& Params, const FVoxelGridSession& Session, FString& OutError)
	{
		int64 ExpectedRevision = 0;
		if (!TryGetOptionalInt64Field(Params, TEXT("expected_revision"), ExpectedRevision))
		{
			return true;
		}

		if (ExpectedRevision != Session.Revision)
		{
			OutError = TEXT("voxel_grid_revision_mismatch");
			return false;
		}
		return true;
	}

	static bool SaveSidecar(const FVoxelGridSession& Session, FString& OutError)
	{
		AActor* Actor = Session.Actor.Get();
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("schema"), TEXT("uai_editable_voxel_grid_v1"));
		Root->SetStringField(TEXT("grid_id"), Session.GridId);
		Root->SetNumberField(TEXT("cell_size"), Session.CellSize);
		Root->SetObjectField(TEXT("grid_origin"), UeAgentModelingOps::MakeVectorJson(Session.GridToWorld.GetLocation()));
		Root->SetObjectField(TEXT("grid_rotation"), UeAgentModelingOps::MakeRotatorJson(Session.GridToWorld.Rotator()));
		TSharedPtr<FJsonObject> AnchorObj = UeAgentModelingOps::MakeVectorJson(Session.AnchorLocal);
		AnchorObj->SetStringField(TEXT("space"), TEXT("grid_local"));
		Root->SetObjectField(TEXT("anchor"), AnchorObj);
		Root->SetNumberField(TEXT("revision"), Session.Revision);
		if (Actor)
		{
			Root->SetStringField(TEXT("actor_name"), Actor->GetName());
			Root->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
			Root->SetStringField(TEXT("actor_path"), Actor->GetPathName());
		}

		TArray<TSharedPtr<FJsonValue>> CellsJson;
		for (const FIntVector& Cell : GetSortedCells(Session.OccupiedCells))
		{
			CellsJson.Add(MakeShared<FJsonValueObject>(MakeIntVectorJson(Cell)));
		}
		Root->SetArrayField(TEXT("cells"), CellsJson);

		FString Serialized;
		if (!UeAgentJsonDiagnostics::SerializeJsonObjectCompact(Root, Serialized))
		{
			OutError = TEXT("voxel_grid_sidecar_serialize_failed");
			return false;
		}

		const FString Directory = FPaths::GetPath(Session.SidecarPath);
		IFileManager::Get().MakeDirectory(*Directory, true);
		if (!FFileHelper::SaveStringToFile(Serialized, *Session.SidecarPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = TEXT("voxel_grid_sidecar_save_failed");
			return false;
		}
		return true;
	}

	bool LoadSidecar(FVoxelGridSession& Session, FString& OutError)
	{
		FString Serialized;
		if (!FFileHelper::LoadFileToString(Serialized, *Session.SidecarPath))
		{
			OutError = TEXT("voxel_grid_sidecar_missing");
			return false;
		}

		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Serialized);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			OutError = TEXT("voxel_grid_sidecar_parse_failed");
			return false;
		}

		FString Schema;
		if (!Root->TryGetStringField(TEXT("schema"), Schema) || Schema != TEXT("uai_editable_voxel_grid_v1"))
		{
			OutError = TEXT("voxel_grid_sidecar_schema_unsupported");
			return false;
		}

		double CellSize = 0.0;
		if (!Root->TryGetNumberField(TEXT("cell_size"), CellSize) || CellSize <= KINDA_SMALL_NUMBER)
		{
			OutError = TEXT("voxel_grid_sidecar_cell_size_invalid");
			return false;
		}

		FVector Origin = FVector::ZeroVector;
		FRotator Rotation = FRotator::ZeroRotator;
		if (!UeAgentModelingOps::TryGetVectorField(Root, TEXT("grid_origin"), Origin) ||
			!UeAgentModelingOps::TryGetRotatorField(Root, TEXT("grid_rotation"), Rotation))
		{
			OutError = TEXT("voxel_grid_sidecar_transform_invalid");
			return false;
		}

		double RevisionValue = 0.0;
		Root->TryGetNumberField(TEXT("revision"), RevisionValue);

		FVector AnchorLocal = FVector::ZeroVector;
		UeAgentModelingOps::TryGetVectorField(Root, TEXT("anchor"), AnchorLocal);

		const TArray<TSharedPtr<FJsonValue>>* CellsJson = nullptr;
		if (!Root->TryGetArrayField(TEXT("cells"), CellsJson) || !CellsJson)
		{
			OutError = TEXT("voxel_grid_sidecar_cells_missing");
			return false;
		}

		TSet<FIntVector> Cells;
		for (const TSharedPtr<FJsonValue>& Value : *CellsJson)
		{
			FVector CellVector = FVector::ZeroVector;
			if (UeAgentJsonDiagnostics::TryReadVectorValue(Value, CellVector))
			{
				Cells.Add(FIntVector(
					static_cast<int32>(FMath::RoundToDouble(CellVector.X)),
					static_cast<int32>(FMath::RoundToDouble(CellVector.Y)),
					static_cast<int32>(FMath::RoundToDouble(CellVector.Z))));
			}
		}

		Session.CellSize = CellSize;
		Session.GridToWorld = FTransform(Rotation, Origin, FVector::OneVector);
		Session.AnchorLocal = AnchorLocal;
		Session.Revision = static_cast<int64>(FMath::RoundToDouble(RevisionValue));
		Session.OccupiedCells = MoveTemp(Cells);
		Session.bDirty = false;
		return true;
	}

	FBox ComputeLocalCellBounds(const TSet<FIntVector>& Cells, double CellSize, const FVector& AnchorLocal)
	{
		FBox Bounds(ForceInit);
		for (const FIntVector& Cell : Cells)
		{
			const FVector Min = FVector(Cell.X * CellSize, Cell.Y * CellSize, Cell.Z * CellSize) - AnchorLocal;
			const FVector Max = Min + FVector(CellSize, CellSize, CellSize);
			Bounds += Min;
			Bounds += Max;
		}
		return Bounds;
	}

	static FVector2f ComputeCubeGridLikeUV(const FVector3d& LocalPosition, const FVector& AnchorLocal, const FVector3d& FaceNormal)
	{
		const FVector3d GridPosition = LocalPosition + FVector3d(AnchorLocal);
		const FVector3d AbsNormal(FMath::Abs(FaceNormal.X), FMath::Abs(FaceNormal.Y), FMath::Abs(FaceNormal.Z));

		FVector2d UV;
		if (AbsNormal.X >= AbsNormal.Y && AbsNormal.X >= AbsNormal.Z)
		{
			UV = FVector2d(GridPosition.Y, GridPosition.Z);
		}
		else if (AbsNormal.Y >= AbsNormal.X && AbsNormal.Y >= AbsNormal.Z)
		{
			UV = FVector2d(GridPosition.X, GridPosition.Z);
		}
		else
		{
			UV = FVector2d(GridPosition.X, GridPosition.Y);
		}

		constexpr double CubeGridWorldSpaceUVScale = 0.01;
		return FVector2f(
			static_cast<float>(UV.X * CubeGridWorldSpaceUVScale),
			static_cast<float>(1.0 - UV.Y * CubeGridWorldSpaceUVScale));
	}

	static void AppendQuad(UE::Geometry::FDynamicMesh3& Mesh, const FVector& A, const FVector& B, const FVector& C, const FVector& D, const FVector& InsidePoint, const FVector& AnchorLocal)
	{
		FVector3d W0(A);
		FVector3d W1(B);
		FVector3d W2(C);
		FVector3d W3(D);
		const FVector3d Inside(InsidePoint);

		FVector3d Cross = (W1 - W0).Cross(W2 - W0);
		const FVector3d FaceCenter = (W0 + W1 + W2 + W3) / 4.0;
		const bool bCrossPointsOutward = Cross.Dot(FaceCenter - Inside) > 0.0;
		if (bCrossPointsOutward)
		{
			Swap(W1, W3);
			Cross = -Cross;
		}

		FVector3d OutwardNormal = -Cross;
		const double NormalLength = OutwardNormal.Length();
		OutwardNormal = (NormalLength > UE_DOUBLE_SMALL_NUMBER) ? (OutwardNormal / NormalLength) : FVector3d::UnitZ();

		const int32 V0 = Mesh.AppendVertex(W0);
		const int32 V1 = Mesh.AppendVertex(W1);
		const int32 V2 = Mesh.AppendVertex(W2);
		const int32 V3 = Mesh.AppendVertex(W3);
		const int32 Tri0 = Mesh.AppendTriangle(V0, V1, V2);
		const int32 Tri1 = Mesh.AppendTriangle(V0, V2, V3);

		if (Mesh.HasAttributes())
		{
			if (UE::Geometry::FDynamicMeshNormalOverlay* NormalOverlay = Mesh.Attributes()->PrimaryNormals())
			{
				const FVector3f Normal((float)OutwardNormal.X, (float)OutwardNormal.Y, (float)OutwardNormal.Z);
				const int32 N0 = NormalOverlay->AppendElement(Normal);
				const int32 N1 = NormalOverlay->AppendElement(Normal);
				const int32 N2 = NormalOverlay->AppendElement(Normal);
				const int32 N3 = NormalOverlay->AppendElement(Normal);
				NormalOverlay->SetTriangle(Tri0, UE::Geometry::FIndex3i(N0, N1, N2));
				NormalOverlay->SetTriangle(Tri1, UE::Geometry::FIndex3i(N0, N2, N3));
			}

			if (UE::Geometry::FDynamicMeshUVOverlay* UVOverlay = Mesh.Attributes()->PrimaryUV())
			{
				const int32 UV0 = UVOverlay->AppendElement(ComputeCubeGridLikeUV(W0, AnchorLocal, OutwardNormal));
				const int32 UV1 = UVOverlay->AppendElement(ComputeCubeGridLikeUV(W1, AnchorLocal, OutwardNormal));
				const int32 UV2 = UVOverlay->AppendElement(ComputeCubeGridLikeUV(W2, AnchorLocal, OutwardNormal));
				const int32 UV3 = UVOverlay->AppendElement(ComputeCubeGridLikeUV(W3, AnchorLocal, OutwardNormal));
				UVOverlay->SetTriangle(Tri0, UE::Geometry::FIndex3i(UV0, UV1, UV2));
				UVOverlay->SetTriangle(Tri1, UE::Geometry::FIndex3i(UV0, UV2, UV3));
			}

			if (UE::Geometry::FDynamicMeshMaterialAttribute* MaterialIDs = Mesh.Attributes()->GetMaterialID())
			{
				MaterialIDs->SetValue(Tri0, 0);
				MaterialIDs->SetValue(Tri1, 0);
			}
		}
	}

	static int32 BuildExposedFaceMesh(const TSet<FIntVector>& Cells, double CellSize, const FVector& AnchorLocal, UE::Geometry::FDynamicMesh3& OutMesh)
	{
		OutMesh.Clear();
		OutMesh.EnableAttributes();
		OutMesh.Attributes()->EnableMaterialID();

		int32 FaceCount = 0;
		for (const FIntVector& Cell : GetSortedCells(Cells))
		{
			const FVector P0 = FVector(Cell.X * CellSize, Cell.Y * CellSize, Cell.Z * CellSize) - AnchorLocal;
			const FVector P1 = P0 + FVector(CellSize, CellSize, CellSize);
			const FVector InsidePoint = (P0 + P1) * 0.5;

			const FVector C000(P0.X, P0.Y, P0.Z);
			const FVector C100(P1.X, P0.Y, P0.Z);
			const FVector C010(P0.X, P1.Y, P0.Z);
			const FVector C110(P1.X, P1.Y, P0.Z);
			const FVector C001(P0.X, P0.Y, P1.Z);
			const FVector C101(P1.X, P0.Y, P1.Z);
			const FVector C011(P0.X, P1.Y, P1.Z);
			const FVector C111(P1.X, P1.Y, P1.Z);

			if (!Cells.Contains(Cell + FIntVector(1, 0, 0)))
			{
				AppendQuad(OutMesh, C100, C110, C111, C101, InsidePoint, AnchorLocal);
				++FaceCount;
			}
			if (!Cells.Contains(Cell + FIntVector(-1, 0, 0)))
			{
				AppendQuad(OutMesh, C010, C000, C001, C011, InsidePoint, AnchorLocal);
				++FaceCount;
			}
			if (!Cells.Contains(Cell + FIntVector(0, 1, 0)))
			{
				AppendQuad(OutMesh, C110, C010, C011, C111, InsidePoint, AnchorLocal);
				++FaceCount;
			}
			if (!Cells.Contains(Cell + FIntVector(0, -1, 0)))
			{
				AppendQuad(OutMesh, C000, C100, C101, C001, InsidePoint, AnchorLocal);
				++FaceCount;
			}
			if (!Cells.Contains(Cell + FIntVector(0, 0, 1)))
			{
				AppendQuad(OutMesh, C001, C101, C111, C011, InsidePoint, AnchorLocal);
				++FaceCount;
			}
			if (!Cells.Contains(Cell + FIntVector(0, 0, -1)))
			{
				AppendQuad(OutMesh, C100, C000, C010, C110, InsidePoint, AnchorLocal);
				++FaceCount;
			}
		}

		return FaceCount;
	}

	static bool TryGetMaterialPathFromParams(const TSharedPtr<FJsonObject>& Params, FString& OutMaterialPath)
	{
		if (!Params.IsValid())
		{
			return false;
		}

		FString MaterialPath;
		if (Params->TryGetStringField(TEXT("material_path"), MaterialPath))
		{
			MaterialPath.TrimStartAndEndInline();
			if (!MaterialPath.IsEmpty())
			{
				OutMaterialPath = MaterialPath;
				return true;
			}
		}

		FString MaterialString;
		if (Params->TryGetStringField(TEXT("material"), MaterialString))
		{
			MaterialString.TrimStartAndEndInline();
			if (!MaterialString.IsEmpty())
			{
				OutMaterialPath = MaterialString;
				return true;
			}
		}

		const TSharedPtr<FJsonObject>* MaterialObj = nullptr;
		if (Params->TryGetObjectField(TEXT("material"), MaterialObj) && MaterialObj && MaterialObj->IsValid())
		{
			if ((*MaterialObj)->TryGetStringField(TEXT("path"), MaterialPath) ||
				(*MaterialObj)->TryGetStringField(TEXT("material_path"), MaterialPath))
			{
				MaterialPath.TrimStartAndEndInline();
				if (!MaterialPath.IsEmpty())
				{
					OutMaterialPath = MaterialPath;
					return true;
				}
			}
		}

		return false;
	}

	static bool IsMaterialAtPackagePath(const UMaterialInterface* Material, const TCHAR* PackagePath)
	{
		if (!Material || !PackagePath || PackagePath[0] == TEXT('\0'))
		{
			return false;
		}

		const FString MaterialPath = Material->GetPathName();
		const FString ExpectedPackagePath(PackagePath);
		return MaterialPath == ExpectedPackagePath || MaterialPath.StartsWith(ExpectedPackagePath + TEXT("."));
	}

	static bool IsReplaceableDefaultVoxelMaterial(const UMaterialInterface* Material)
	{
		const UMaterialInterface* EngineDefaultMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
		return Material == nullptr ||
			Material == EngineDefaultMaterial ||
			(EngineDefaultMaterial != nullptr && Material->GetPathName() == EngineDefaultMaterial->GetPathName()) ||
			IsMaterialAtPackagePath(Material, LegacyCheckerVoxelGridMaterialPath) ||
			IsMaterialAtPackagePath(Material, LegacyWorkingVoxelGridMaterialPath) ||
			IsMaterialAtPackagePath(Material, LegacySimpleBaseVoxelGridMaterialPath);
	}

	static bool ResolveVoxelGridMaterial(
		const TSharedPtr<FJsonObject>& Params,
		UDynamicMeshComponent* Component,
		UMaterialInterface*& OutMaterial,
		FString& OutMaterialPath,
		FString& OutMaterialSource,
		FString& OutError)
	{
		OutMaterial = nullptr;
		OutMaterialPath.Reset();
		OutMaterialSource.Reset();

		FString RequestedMaterialPath;
		if (TryGetMaterialPathFromParams(Params, RequestedMaterialPath))
		{
			UMaterialInterface* RequestedMaterial = LoadObject<UMaterialInterface>(nullptr, *RequestedMaterialPath);
			if (!RequestedMaterial)
			{
				OutError = TEXT("voxel_grid_material_not_found");
				return false;
			}

			OutMaterial = RequestedMaterial;
			OutMaterialPath = RequestedMaterial->GetPathName();
			OutMaterialSource = TEXT("explicit");
			return true;
		}

		if (Component)
		{
			UMaterialInterface* ExistingMaterial = Component->GetMaterial(0);
			if (!IsReplaceableDefaultVoxelMaterial(ExistingMaterial))
			{
				OutMaterial = ExistingMaterial;
				OutMaterialPath = ExistingMaterial->GetPathName();
				OutMaterialSource = TEXT("existing_component");
				return true;
			}
		}

		OutMaterial = nullptr;
		OutMaterialPath.Reset();
		OutMaterialSource = TEXT("cubegrid_component_default");
		return true;
	}

	static bool AreCellsPresentInSlab(const TSet<FIntVector>& Cells, const FIntVector& Min, const FIntVector& MaxExclusive)
	{
		for (int32 Z = Min.Z; Z < MaxExclusive.Z; ++Z)
		{
			for (int32 Y = Min.Y; Y < MaxExclusive.Y; ++Y)
			{
				for (int32 X = Min.X; X < MaxExclusive.X; ++X)
				{
					if (!Cells.Contains(FIntVector(X, Y, Z)))
					{
						return false;
					}
				}
			}
		}
		return true;
	}

	static TArray<FCollisionBoxRange> BuildExactCoverBoxes(const TSet<FIntVector>& Cells)
	{
		TSet<FIntVector> Remaining = Cells;
		TArray<FCollisionBoxRange> Boxes;

		while (Remaining.Num() > 0)
		{
			const TArray<FIntVector> Sorted = GetSortedCells(Remaining);
			const FIntVector Start = Sorted[0];

			FIntVector Min = Start;
			FIntVector MaxExclusive = Start + FIntVector(1, 1, 1);

			while (AreCellsPresentInSlab(Remaining, FIntVector(MaxExclusive.X, Min.Y, Min.Z), FIntVector(MaxExclusive.X + 1, MaxExclusive.Y, MaxExclusive.Z)))
			{
				++MaxExclusive.X;
			}

			while (AreCellsPresentInSlab(Remaining, FIntVector(Min.X, MaxExclusive.Y, Min.Z), FIntVector(MaxExclusive.X, MaxExclusive.Y + 1, MaxExclusive.Z)))
			{
				++MaxExclusive.Y;
			}

			while (AreCellsPresentInSlab(Remaining, FIntVector(Min.X, Min.Y, MaxExclusive.Z), FIntVector(MaxExclusive.X, MaxExclusive.Y, MaxExclusive.Z + 1)))
			{
				++MaxExclusive.Z;
			}

			FCollisionBoxRange Box;
			Box.Min = Min;
			Box.MaxExclusive = MaxExclusive;
			Boxes.Add(Box);

			for (int32 Z = Min.Z; Z < MaxExclusive.Z; ++Z)
			{
				for (int32 Y = Min.Y; Y < MaxExclusive.Y; ++Y)
				{
					for (int32 X = Min.X; X < MaxExclusive.X; ++X)
					{
						Remaining.Remove(FIntVector(X, Y, Z));
					}
				}
			}
		}

		return Boxes;
	}

	static int64 GetBoxCellCount(const FCollisionBoxRange& Box)
	{
		return static_cast<int64>(Box.MaxExclusive.X - Box.Min.X) *
			static_cast<int64>(Box.MaxExclusive.Y - Box.Min.Y) *
			static_cast<int64>(Box.MaxExclusive.Z - Box.Min.Z);
	}

	static ECollisionEnabled::Type ParseCollisionEnabled(const FString& Value)
	{
		const FString Normalized = Value.TrimStartAndEnd().ToLower();
		if (Normalized == TEXT("none") || Normalized == TEXT("no_collision"))
		{
			return ECollisionEnabled::NoCollision;
		}
		if (Normalized == TEXT("query_only"))
		{
			return ECollisionEnabled::QueryOnly;
		}
		if (Normalized == TEXT("physics_only"))
		{
			return ECollisionEnabled::PhysicsOnly;
		}
		return ECollisionEnabled::QueryAndPhysics;
	}

	static bool CreateCollisionBoxes(AActor* Actor, const FVoxelGridSession& Session, const TArray<FCollisionBoxRange>& Boxes, const FString& ProfileName, ECollisionEnabled::Type CollisionEnabled, int32 MaxBoxes, TArray<TSharedPtr<FJsonValue>>& OutComponents, int64& OutCoveredCells, FString& OutError)
	{
		if (!Actor)
		{
			OutError = TEXT("voxel_grid_actor_invalid");
			return false;
		}

		if (Boxes.Num() > MaxBoxes)
		{
			OutError = TEXT("voxel_grid_collision_box_limit_exceeded");
			return false;
		}

		USceneComponent* Root = Actor->GetRootComponent();
		if (!Root)
		{
			OutError = TEXT("voxel_grid_actor_root_missing");
			return false;
		}

		const FName CollisionTagName(*MakeCollisionTag(Session.GridId));
		for (int32 Index = 0; Index < Boxes.Num(); ++Index)
		{
			const FCollisionBoxRange& Box = Boxes[Index];
			const FVector LocalMin = FVector(Box.Min.X * Session.CellSize, Box.Min.Y * Session.CellSize, Box.Min.Z * Session.CellSize) - Session.AnchorLocal;
			const FVector LocalMax(Box.MaxExclusive.X * Session.CellSize, Box.MaxExclusive.Y * Session.CellSize, Box.MaxExclusive.Z * Session.CellSize);
			const FVector ShiftedLocalMax = LocalMax - Session.AnchorLocal;
			const FVector Center = (LocalMin + ShiftedLocalMax) * 0.5;
			const FVector Extent = (ShiftedLocalMax - LocalMin) * 0.5;

			UBoxComponent* BoxComponent = NewObject<UBoxComponent>(Actor, *FString::Printf(TEXT("UAI_VoxelCollision_%03d"), Index), RF_Transactional);
			if (!BoxComponent)
			{
				OutError = TEXT("voxel_grid_collision_component_create_failed");
				return false;
			}

			BoxComponent->SetMobility(EComponentMobility::Movable);
			BoxComponent->SetupAttachment(Root);
			BoxComponent->SetRelativeLocation(Center);
			BoxComponent->SetRelativeRotation(FRotator::ZeroRotator);
			BoxComponent->SetRelativeScale3D(FVector::OneVector);
			BoxComponent->SetBoxExtent(Extent, false);
			BoxComponent->SetCollisionProfileName(FName(*ProfileName));
			BoxComponent->SetCollisionEnabled(CollisionEnabled);
			BoxComponent->SetHiddenInGame(true);
			BoxComponent->SetVisibility(false, true);
			BoxComponent->ComponentTags.AddUnique(CollisionTagName);
			Actor->AddInstanceComponent(BoxComponent);
			BoxComponent->RegisterComponent();
			BoxComponent->UpdateOverlaps();

			OutCoveredCells += GetBoxCellCount(Box);

			TSharedPtr<FJsonObject> ComponentObj = MakeShared<FJsonObject>();
			ComponentObj->SetStringField(TEXT("name"), BoxComponent->GetName());
			ComponentObj->SetObjectField(TEXT("cell_range"), MakeBoxRangeJson(Box));
			ComponentObj->SetObjectField(TEXT("relative_center"), UeAgentModelingOps::MakeVectorJson(Center));
			ComponentObj->SetObjectField(TEXT("extent"), UeAgentModelingOps::MakeVectorJson(Extent));
			OutComponents.Add(MakeShared<FJsonValueObject>(ComponentObj));
		}

		return true;
	}

	static bool TryGetSession(const TSharedPtr<FJsonObject>& Params, FVoxelGridSession*& OutSession, FString& OutError)
	{
		FString SessionId;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("session_id"), SessionId) || SessionId.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("session_id_required");
			return false;
		}

		FVoxelGridSession* Session = GVoxelGridSessions.Find(SessionId.TrimStartAndEnd());
		if (!Session)
		{
			OutError = TEXT("voxel_grid_session_not_found");
			return false;
		}

		if (!Session->Actor.IsValid())
		{
			OutError = TEXT("voxel_grid_actor_invalid");
			return false;
		}

		OutSession = Session;
		return true;
	}

	FVoxelGridSession* FindActiveSessionForActor(AActor* Actor)
	{
		if (!Actor)
		{
			return nullptr;
		}

		for (TPair<FString, FVoxelGridSession>& Pair : GVoxelGridSessions)
		{
			if (Pair.Value.Actor.Get() == Actor)
			{
				return &Pair.Value;
			}
		}
		return nullptr;
	}
}

bool FUeAgentHttpServer::CmdModelingVoxelGridBegin(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentModelingOps;
	using namespace UeAgentModelingVoxelGrid;

	OutData = MakeShared<FJsonObject>();
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Params = Ctx.Params.IsValid() ? Ctx.Params : MakeShared<FJsonObject>();
	const FString Mode = GetOptionalStringField(Params, TEXT("mode"), TEXT("create")).ToLower();
	FString SessionId = GetOptionalStringField(Params, TEXT("session_id"), FString());
	if (SessionId.IsEmpty())
	{
		SessionId = MakeNewSessionId();
	}

	FString GridId = GetOptionalStringField(Params, TEXT("grid_id"), FString());
	AActor* Actor = nullptr;
	double CellSize = 100.0;
	FVector Origin = FVector::ZeroVector;
	FRotator Rotation = FRotator::ZeroRotator;

	if (Mode == TEXT("create"))
	{
		if (GridId.IsEmpty())
		{
			GridId = MakeNewGridId();
		}

		if (!TryParseGridParams(Params, CellSize, Origin, Rotation, OutError))
		{
			return false;
		}

		FActorSpawnParameters SpawnParams;
		SpawnParams.ObjectFlags = RF_Transactional;
		SpawnParams.Name = MakeUniqueVoxelGridActorName(World, GridId);
		Actor = World->SpawnActor<ADynamicMeshActor>(ADynamicMeshActor::StaticClass(), Origin, Rotation, SpawnParams);
		if (!Actor)
		{
			OutError = TEXT("voxel_grid_actor_spawn_failed");
			return false;
		}

		Actor->SetActorScale3D(FVector::OneVector);
		ApplyOptionalCreatedActorMetadata(Actor, Params);
	}
	else if (Mode == TEXT("edit") || Mode == TEXT("modify"))
	{
		if (!TryResolveVoxelActor(World, Params, Actor, OutError))
		{
			return false;
		}

		if (GridId.IsEmpty() && !TryGetGridIdFromActor(Actor, GridId))
		{
			OutError = TEXT("voxel_grid_id_required_or_actor_tag_missing");
			return false;
		}
	}
	else
	{
		OutError = TEXT("unsupported_voxel_grid_begin_mode");
		return false;
	}

	FVoxelGridSession Session;
	Session.SessionId = SessionId;
	Session.GridId = GridId;
	Session.Actor = Actor;
	Session.SidecarPath = GetVoxelGridSidecarPath(GridId);

	if (Mode == TEXT("create"))
	{
		Session.CellSize = CellSize;
		Session.GridToWorld = FTransform(Rotation, Origin, FVector::OneVector);
		Session.AnchorLocal = FVector::ZeroVector;
		Session.Revision = 0;
		Session.bDirty = true;
		Actor->Modify();
		Actor->SetActorTransform(MakeActorTransformFromGridAndAnchor(Session), false, nullptr, ETeleportType::TeleportPhysics);
		SetActorGridTag(Actor, GridId);
	}
	else
	{
		SetActorGridTag(Actor, GridId);
		if (!LoadSidecar(Session, OutError))
		{
			return false;
		}
		Actor->Modify();
		Actor->SetActorTransform(MakeActorTransformFromGridAndAnchor(Session), false, nullptr, ETeleportType::TeleportPhysics);
	}

	if (UDynamicMeshComponent* DynamicMeshComponent = FindDynamicMeshComponent(Actor))
	{
		DynamicMeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}

	Actor->MarkPackageDirty();
	GVoxelGridSessions.Add(SessionId, MoveTemp(Session));
	FVoxelGridSession& StoredSession = GVoxelGridSessions.FindChecked(SessionId);
	SelectActors({ Actor });

	OutData->SetStringField(TEXT("session_id"), StoredSession.SessionId);
	OutData->SetStringField(TEXT("grid_id"), StoredSession.GridId);
	OutData->SetStringField(TEXT("mode"), Mode);
	OutData->SetStringField(TEXT("actor_id"), Actor->GetName());
	OutData->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
	OutData->SetStringField(TEXT("output_type"), TEXT("dynamic_mesh_actor"));
	OutData->SetNumberField(TEXT("cell_size"), StoredSession.CellSize);
	OutData->SetNumberField(TEXT("revision"), StoredSession.Revision);
	OutData->SetNumberField(TEXT("occupied_cell_count"), StoredSession.OccupiedCells.Num());
	OutData->SetBoolField(TEXT("dirty"), StoredSession.bDirty);
	OutData->SetStringField(TEXT("sidecar_path"), StoredSession.SidecarPath);
	OutData->SetObjectField(TEXT("grid_origin"), MakeVectorJson(StoredSession.GridToWorld.GetLocation()));
	OutData->SetObjectField(TEXT("grid_rotation"), MakeRotatorJson(StoredSession.GridToWorld.Rotator()));
	TSharedPtr<FJsonObject> AnchorObj = MakeVectorJson(StoredSession.AnchorLocal);
	AnchorObj->SetStringField(TEXT("space"), TEXT("grid_local"));
	OutData->SetObjectField(TEXT("anchor"), AnchorObj);
	return true;
}

bool FUeAgentHttpServer::CmdModelingVoxelGridAddBounds(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentModelingVoxelGrid;

	OutData = MakeShared<FJsonObject>();
	FVoxelGridSession* Session = nullptr;
	if (!TryGetSession(Ctx.Params, Session, OutError) || !CheckExpectedRevision(Ctx.Params, *Session, OutError))
	{
		return false;
	}

	FCellRange Range;
	TSharedPtr<FJsonObject> BoundsReadback;
	if (!TryParseBoundsRange(Ctx.Params, Session->GridToWorld, Session->AnchorLocal, Session->CellSize, Range, BoundsReadback, OutError))
	{
		return false;
	}

	int32 AddedCount = 0;
	for (int32 Z = Range.Min.Z; Z < Range.MaxExclusive.Z; ++Z)
	{
		for (int32 Y = Range.Min.Y; Y < Range.MaxExclusive.Y; ++Y)
		{
			for (int32 X = Range.Min.X; X < Range.MaxExclusive.X; ++X)
			{
				if (!Session->OccupiedCells.Contains(FIntVector(X, Y, Z)))
				{
					Session->OccupiedCells.Add(FIntVector(X, Y, Z));
					++AddedCount;
				}
			}
		}
	}

	++Session->Revision;
	Session->bDirty = true;

	OutData->SetStringField(TEXT("session_id"), Session->SessionId);
	OutData->SetStringField(TEXT("grid_id"), Session->GridId);
	OutData->SetStringField(TEXT("operation"), TEXT("add_bounds"));
	OutData->SetNumberField(TEXT("changed_cell_count"), AddedCount);
	OutData->SetNumberField(TEXT("occupied_cell_count"), Session->OccupiedCells.Num());
	OutData->SetNumberField(TEXT("revision"), Session->Revision);
	OutData->SetObjectField(TEXT("bounds"), BoundsReadback);
	return true;
}

bool FUeAgentHttpServer::CmdModelingVoxelGridRemoveBounds(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentModelingVoxelGrid;

	OutData = MakeShared<FJsonObject>();
	FVoxelGridSession* Session = nullptr;
	if (!TryGetSession(Ctx.Params, Session, OutError) || !CheckExpectedRevision(Ctx.Params, *Session, OutError))
	{
		return false;
	}

	FCellRange Range;
	TSharedPtr<FJsonObject> BoundsReadback;
	if (!TryParseBoundsRange(Ctx.Params, Session->GridToWorld, Session->AnchorLocal, Session->CellSize, Range, BoundsReadback, OutError))
	{
		return false;
	}

	int32 RemovedCount = 0;
	for (int32 Z = Range.Min.Z; Z < Range.MaxExclusive.Z; ++Z)
	{
		for (int32 Y = Range.Min.Y; Y < Range.MaxExclusive.Y; ++Y)
		{
			for (int32 X = Range.Min.X; X < Range.MaxExclusive.X; ++X)
			{
				RemovedCount += Session->OccupiedCells.Remove(FIntVector(X, Y, Z));
			}
		}
	}

	++Session->Revision;
	Session->bDirty = true;

	OutData->SetStringField(TEXT("session_id"), Session->SessionId);
	OutData->SetStringField(TEXT("grid_id"), Session->GridId);
	OutData->SetStringField(TEXT("operation"), TEXT("remove_bounds"));
	OutData->SetNumberField(TEXT("changed_cell_count"), RemovedCount);
	OutData->SetNumberField(TEXT("occupied_cell_count"), Session->OccupiedCells.Num());
	OutData->SetNumberField(TEXT("revision"), Session->Revision);
	OutData->SetObjectField(TEXT("bounds"), BoundsReadback);
	return true;
}

bool FUeAgentHttpServer::CmdModelingVoxelGridApply(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentModelingOps;
	using namespace UeAgentModelingVoxelGrid;

	OutData = MakeShared<FJsonObject>();
	FVoxelGridSession* Session = nullptr;
	if (!TryGetSession(Ctx.Params, Session, OutError) || !CheckExpectedRevision(Ctx.Params, *Session, OutError))
	{
		return false;
	}

	AActor* Actor = Session->Actor.Get();
	UDynamicMeshComponent* DynamicMeshComponent = FindDynamicMeshComponent(Actor);
	if (!Actor || !DynamicMeshComponent)
	{
		OutError = TEXT("voxel_grid_dynamic_mesh_component_required");
		return false;
	}

	FString MeshBuildMode = GetOptionalStringField(Ctx.Params, TEXT("mesh_build"), TEXT("exposed_faces")).ToLower();
	if (MeshBuildMode != TEXT("exposed_faces"))
	{
		OutError = TEXT("unsupported_voxel_grid_mesh_build_mode");
		return false;
	}

	TSharedPtr<FJsonObject> CollisionObj;
	TryGetObjectField(Ctx.Params, TEXT("collision"), CollisionObj);
	FString CollisionMode = TEXT("merged_box_components");
	if (CollisionObj.IsValid())
	{
		FString ModeValue;
		if (CollisionObj->TryGetStringField(TEXT("mode"), ModeValue))
		{
			CollisionMode = ModeValue.TrimStartAndEnd().ToLower();
		}
	}
	else
	{
		FString ModeValue;
		if (Ctx.Params.IsValid() && Ctx.Params->TryGetStringField(TEXT("collision"), ModeValue))
		{
			CollisionMode = ModeValue.TrimStartAndEnd().ToLower();
		}
	}

	FString CollisionProfile = TEXT("BlockAll");
	if (CollisionObj.IsValid())
	{
		CollisionObj->TryGetStringField(TEXT("profile_name"), CollisionProfile);
	}

	FString CollisionEnabledString = TEXT("query_and_physics");
	if (CollisionObj.IsValid())
	{
		CollisionObj->TryGetStringField(TEXT("enabled"), CollisionEnabledString);
	}

	int64 MaxBoxesValue = DefaultMaxCollisionBoxes;
	if (CollisionObj.IsValid())
	{
		TryGetOptionalInt64Field(CollisionObj, TEXT("max_boxes"), MaxBoxesValue);
	}
	const int32 MaxBoxes = static_cast<int32>(FMath::Max<int64>(0, MaxBoxesValue));
	const bool bReplaceExistingCollision = !CollisionObj.IsValid() || !CollisionObj->HasField(TEXT("replace_existing")) || CollisionObj->GetBoolField(TEXT("replace_existing"));

	const bool bCollisionDisabled = CollisionMode == TEXT("none") || CollisionMode == TEXT("disabled");
	const bool bCollisionBoxesEnabled = CollisionMode == TEXT("merged_box_components") || CollisionMode == TEXT("exact_box_components");
	if (!bCollisionDisabled && !bCollisionBoxesEnabled)
	{
		OutError = TEXT("unsupported_voxel_grid_collision_mode");
		return false;
	}

	TArray<FCollisionBoxRange> Boxes;
	if (bCollisionBoxesEnabled)
	{
		Boxes = BuildExactCoverBoxes(Session->OccupiedCells);
		if (Boxes.Num() > MaxBoxes)
		{
			OutError = TEXT("voxel_grid_collision_box_limit_exceeded");
			return false;
		}
	}

	UE::Geometry::FDynamicMesh3 Mesh;
	const int32 ExposedFaceCount = BuildExposedFaceMesh(Session->OccupiedCells, Session->CellSize, Session->AnchorLocal, Mesh);
	const int32 VertexCount = Mesh.VertexCount();
	const int32 TriangleCount = Mesh.TriangleCount();

	UMaterialInterface* AppliedMaterial = nullptr;
	FString AppliedMaterialPath;
	FString AppliedMaterialSource;
	if (!ResolveVoxelGridMaterial(Ctx.Params, DynamicMeshComponent, AppliedMaterial, AppliedMaterialPath, AppliedMaterialSource, OutError))
	{
		return false;
	}

	DynamicMeshComponent->Modify();
	DynamicMeshComponent->SetMesh(MoveTemp(Mesh));
	DynamicMeshComponent->SetMaterial(0, AppliedMaterial);
	DynamicMeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	DynamicMeshComponent->MarkRenderStateDirty();

	if (bReplaceExistingCollision)
	{
		RemoveVoxelCollisionComponents(Actor, Session->GridId);
	}

	TArray<TSharedPtr<FJsonValue>> CollisionComponents;
	int64 CoveredCellCount = 0;
	bool bCollisionGenerated = false;
	bool bCollisionCoverageMatches = true;
	int32 CollisionBoxCount = 0;

	if (bCollisionDisabled)
	{
		bCollisionGenerated = false;
	}
	else if (bCollisionBoxesEnabled)
	{
		CollisionBoxCount = Boxes.Num();
		if (!CreateCollisionBoxes(Actor, *Session, Boxes, CollisionProfile, ParseCollisionEnabled(CollisionEnabledString), MaxBoxes, CollisionComponents, CoveredCellCount, OutError))
		{
			return false;
		}
		bCollisionGenerated = Boxes.Num() > 0;
		bCollisionCoverageMatches = CoveredCellCount == Session->OccupiedCells.Num();
	}

	FBox LocalBounds = ComputeLocalCellBounds(Session->OccupiedCells, Session->CellSize, Session->AnchorLocal);
	FBox WorldBounds = TransformBoxBy(ComputeLocalCellBounds(Session->OccupiedCells, Session->CellSize, FVector::ZeroVector), Session->GridToWorld);

	if (!SaveSidecar(*Session, OutError))
	{
		return false;
	}

	Actor->Modify();
	Actor->SetActorTransform(MakeActorTransformFromGridAndAnchor(*Session), false, nullptr, ETeleportType::TeleportPhysics);
	SetActorGridTag(Actor, Session->GridId);
	Actor->MarkPackageDirty();

	const bool bKeepSession = Ctx.Params.IsValid() && Ctx.Params->HasField(TEXT("keep_session")) && Ctx.Params->GetBoolField(TEXT("keep_session"));
	Session->bDirty = false;

	OutData->SetStringField(TEXT("session_id"), Session->SessionId);
	OutData->SetStringField(TEXT("grid_id"), Session->GridId);
	OutData->SetStringField(TEXT("actor_id"), Actor->GetName());
	OutData->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
	OutData->SetStringField(TEXT("output_type"), TEXT("dynamic_mesh_actor"));
	OutData->SetNumberField(TEXT("revision"), Session->Revision);
	OutData->SetNumberField(TEXT("occupied_cell_count"), Session->OccupiedCells.Num());
	OutData->SetBoolField(TEXT("session_closed"), !bKeepSession);

	TSharedPtr<FJsonObject> MeshObj = MakeShared<FJsonObject>();
	MeshObj->SetStringField(TEXT("build_mode"), MeshBuildMode);
	MeshObj->SetNumberField(TEXT("exposed_face_count"), ExposedFaceCount);
	MeshObj->SetNumberField(TEXT("vertex_count"), VertexCount);
	MeshObj->SetNumberField(TEXT("triangle_count"), TriangleCount);
	MeshObj->SetObjectField(TEXT("local_bounds"), MakeFBoxJson(LocalBounds));
	MeshObj->SetObjectField(TEXT("world_bounds"), MakeFBoxJson(WorldBounds));
	TSharedPtr<FJsonObject> MaterialObj = MakeShared<FJsonObject>();
	MaterialObj->SetStringField(TEXT("path"), AppliedMaterialPath);
	MaterialObj->SetStringField(TEXT("source"), AppliedMaterialSource);
	MaterialObj->SetStringField(TEXT("default_path"), TEXT(""));
	MaterialObj->SetBoolField(TEXT("uses_cubegrid_component_default"), AppliedMaterialSource == TEXT("cubegrid_component_default"));
	MaterialObj->SetBoolField(TEXT("preserves_existing_custom_material"), AppliedMaterialSource == TEXT("existing_component"));
	MeshObj->SetObjectField(TEXT("material"), MaterialObj);
	OutData->SetObjectField(TEXT("mesh"), MeshObj);

	TSharedPtr<FJsonObject> CollisionReadback = MakeShared<FJsonObject>();
	CollisionReadback->SetStringField(TEXT("mode"), CollisionMode);
	CollisionReadback->SetBoolField(TEXT("generated"), bCollisionGenerated);
	CollisionReadback->SetStringField(TEXT("profile_name"), CollisionProfile);
	CollisionReadback->SetStringField(TEXT("enabled"), CollisionEnabledString);
	CollisionReadback->SetNumberField(TEXT("box_count"), CollisionBoxCount);
	CollisionReadback->SetNumberField(TEXT("covered_cell_count"), CoveredCellCount);
	CollisionReadback->SetBoolField(TEXT("coverage_matches_occupied_cells"), bCollisionCoverageMatches);
	CollisionReadback->SetArrayField(TEXT("components"), CollisionComponents);
	OutData->SetObjectField(TEXT("collision"), CollisionReadback);

	TSharedPtr<FJsonObject> MetadataObj = MakeShared<FJsonObject>();
	MetadataObj->SetBoolField(TEXT("saved"), true);
	MetadataObj->SetStringField(TEXT("sidecar_path"), Session->SidecarPath);
	TSharedPtr<FJsonObject> MetadataAnchorObj = MakeVectorJson(Session->AnchorLocal);
	MetadataAnchorObj->SetStringField(TEXT("space"), TEXT("grid_local"));
	MetadataObj->SetObjectField(TEXT("anchor"), MetadataAnchorObj);
	OutData->SetObjectField(TEXT("metadata"), MetadataObj);

	if (!bKeepSession)
	{
		GVoxelGridSessions.Remove(Session->SessionId);
	}

	return true;
}

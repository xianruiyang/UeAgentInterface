#include "DomainAdapters/ModelingGeometry/UeAgentModelingVoxelGrid.h"

#include "Components/BoxComponent.h"
#include "Components/DynamicMeshComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMesh/DynamicMeshOverlay.h"
#include "DynamicMeshActor.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UDynamicMesh.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::DomainAdapters::ModelingGeometry::VoxelGrid
{
	using namespace CommandEdge;
	using UE::Geometry::FDynamicMesh3;
	using UE::Geometry::FDynamicMeshMaterialAttribute;
	using UE::Geometry::FDynamicMeshNormalOverlay;
	using UE::Geometry::FDynamicMeshUVOverlay;
	using UE::Geometry::FIndex3i;

	namespace
	{
		constexpr const TCHAR* ActorTagPrefix = TEXT("UAIEditableVoxelGrid:");
		constexpr const TCHAR* CollisionTagPrefix =
			TEXT("UAIEditableVoxelGridCollision:");
		constexpr const TCHAR* CollisionGenerationTagPrefix =
			TEXT("UAIEditableVoxelGridCollisionGeneration:");
		constexpr const TCHAR* SidecarSchema = TEXT("uai.modeling.voxel_grid.v2");
		constexpr const TCHAR* LegacySidecarSchema =
			TEXT("uai_editable_voxel_grid_v1");
		constexpr int64 MaxOperationCells = 200000;
		constexpr int64 MaxSessionCells = 500000;
		constexpr int32 MaxSessions = 64;
		constexpr int32 MaxCollisionBoxes = 2048;

		struct FFailure final
		{
			FString Code;
			FString Detail;
			FString TerminalStatus = TEXT("not_started");
			bool bRetrySafe = true;
			bool bMutationStarted = false;
		};

		struct FSession final
		{
			FString SessionId;
			FString GridId;
			TWeakObjectPtr<ADynamicMeshActor> Actor;
			double CellSize = 100.0;
			FTransform GridToWorld = FTransform::Identity;
			FVector AnchorLocal = FVector::ZeroVector;
			TSet<FIntVector> Cells;
			int64 Revision = 0;
			bool bDirty = false;
			bool bCreatedActor = false;
			bool bCommitted = false;
			FString SidecarPath;
			FString CollisionGeneration;
			FString CollisionMode = TEXT("none");
		};

		FString MakeGuid(const TCHAR* Prefix)
		{
			return FString(Prefix)
				+ FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(16);
		}

		void SetFailure(
			FFailure& OutFailure,
			const FString& Code,
			const FString& Detail = FString(),
			const FString& TerminalStatus = TEXT("not_started"),
			const bool bRetrySafe = true,
			const bool bMutationStarted = false)
		{
			OutFailure.Code = Code;
			OutFailure.Detail = Detail;
			OutFailure.TerminalStatus = TerminalStatus;
			OutFailure.bRetrySafe = bRetrySafe;
			OutFailure.bMutationStarted = bMutationStarted;
		}

		FString JsonText(const TSharedRef<FJsonObject>& Object)
		{
			FString Text;
			const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>>
				Writer = TJsonWriterFactory<TCHAR,
					TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
			FJsonSerializer::Serialize(Object, Writer);
			return Text;
		}

		FString ErrorJson(const FString& SubjectId, const FFailure& Failure)
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("code"), Failure.Code);
			Object->SetStringField(TEXT("subject_id"), SubjectId);
			Object->SetStringField(TEXT("detail"), Failure.Detail);
			Object->SetStringField(
				TEXT("terminal_status"), Failure.TerminalStatus);
			Object->SetBoolField(TEXT("retry_safe"), Failure.bRetrySafe);
			Object->SetBoolField(
				TEXT("mutation_started"), Failure.bMutationStarted);
			return JsonText(Object);
		}

		TSharedRef<FJsonObject> VectorJson(const FVector& Value)
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetNumberField(TEXT("x"), Value.X);
			Object->SetNumberField(TEXT("y"), Value.Y);
			Object->SetNumberField(TEXT("z"), Value.Z);
			return Object;
		}

		TSharedRef<FJsonObject> RotatorJson(const FRotator& Value)
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetNumberField(TEXT("pitch"), Value.Pitch);
			Object->SetNumberField(TEXT("yaw"), Value.Yaw);
			Object->SetNumberField(TEXT("roll"), Value.Roll);
			return Object;
		}

		TSharedRef<FJsonObject> IntVectorJson(const FIntVector& Value)
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetNumberField(TEXT("x"), Value.X);
			Object->SetNumberField(TEXT("y"), Value.Y);
			Object->SetNumberField(TEXT("z"), Value.Z);
			return Object;
		}

		TSharedRef<FJsonObject> RangeJson(const FVoxelGridCellRange& Range)
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetObjectField(TEXT("min"), IntVectorJson(Range.Min));
			Object->SetObjectField(
				TEXT("max_exclusive"), IntVectorJson(Range.MaxExclusive));
			Object->SetNumberField(TEXT("cell_count"), Range.NumCells());
			return Object;
		}

		bool IsFiniteVector(const FVector& Value)
		{
			return FMath::IsFinite(Value.X)
				&& FMath::IsFinite(Value.Y)
				&& FMath::IsFinite(Value.Z);
		}

		bool TryReadVector(
			const TSharedPtr<FJsonObject>& Parent,
			const TCHAR* Field,
			FVector& OutValue)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			double X = 0.0;
			double Y = 0.0;
			double Z = 0.0;
			if (!Parent.IsValid()
				|| !Parent->TryGetObjectField(Field, Object)
				|| !Object || !Object->IsValid()
				|| !(*Object)->TryGetNumberField(TEXT("x"), X)
				|| !(*Object)->TryGetNumberField(TEXT("y"), Y)
				|| !(*Object)->TryGetNumberField(TEXT("z"), Z))
			{
				return false;
			}
			OutValue = FVector(X, Y, Z);
			return IsFiniteVector(OutValue);
		}

		bool TryReadRotator(
			const TSharedPtr<FJsonObject>& Parent,
			const TCHAR* Field,
			FRotator& OutValue)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			double Pitch = 0.0;
			double Yaw = 0.0;
			double Roll = 0.0;
			if (!Parent.IsValid()
				|| !Parent->TryGetObjectField(Field, Object)
				|| !Object || !Object->IsValid()
				|| !(*Object)->TryGetNumberField(TEXT("pitch"), Pitch)
				|| !(*Object)->TryGetNumberField(TEXT("yaw"), Yaw)
				|| !(*Object)->TryGetNumberField(TEXT("roll"), Roll)
				|| !FMath::IsFinite(Pitch)
				|| !FMath::IsFinite(Yaw)
				|| !FMath::IsFinite(Roll))
			{
				return false;
			}
			OutValue = FRotator(Pitch, Yaw, Roll);
			return true;
		}

		bool TryReadIntegralField(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field,
			int64& OutValue)
		{
			double Number = 0.0;
			if (!Object.IsValid()
				|| !Object->TryGetNumberField(Field, Number)
				|| !FMath::IsFinite(Number)
				|| !FMath::IsNearlyEqual(Number, FMath::RoundToDouble(Number), 1.e-6)
				|| Number < static_cast<double>(MIN_int64)
				|| Number > static_cast<double>(MAX_int64))
			{
				return false;
			}
			OutValue = static_cast<int64>(FMath::RoundToDouble(Number));
			return true;
		}

		bool IsSafeIdentity(const FString& Value)
		{
			if (Value.IsEmpty() || Value.Len() > 96 || Value.Contains(TEXT("..")))
			{
				return false;
			}
			for (const TCHAR Character : Value)
			{
				if (!FChar::IsAlnum(Character)
					&& Character != TEXT('_')
					&& Character != TEXT('-')
					&& Character != TEXT('.'))
				{
					return false;
				}
			}
			return true;
		}

		FString ActorTag(const FString& GridId)
		{
			return FString(ActorTagPrefix) + GridId;
		}

		FString CollisionTag(const FString& GridId)
		{
			return FString(CollisionTagPrefix) + GridId;
		}

		FString CollisionGenerationTag(
			const FString& GridId,
			const FString& Generation)
		{
			return FString(CollisionGenerationTagPrefix)
				+ GridId + TEXT(":") + Generation;
		}

		FString SidecarPath(const FString& GridId)
		{
			return FPaths::Combine(
				FPaths::ProjectSavedDir(),
				TEXT("UeAgentInterface"),
				TEXT("VoxelGrids"),
				GridId + TEXT(".json"));
		}

		FTransform ActorTransform(const FSession& Session)
		{
			FTransform Result = Session.GridToWorld;
			Result.SetLocation(
				Session.GridToWorld.TransformPosition(Session.AnchorLocal));
			Result.SetScale3D(FVector::OneVector);
			return Result;
		}

		TArray<FIntVector> SortedCells(const TSet<FIntVector>& Cells)
		{
			TArray<FIntVector> Result = Cells.Array();
			Result.Sort([](const FIntVector& Left, const FIntVector& Right)
			{
				if (Left.Z != Right.Z) return Left.Z < Right.Z;
				if (Left.Y != Right.Y) return Left.Y < Right.Y;
				return Left.X < Right.X;
			});
			return Result;
		}

		bool SameCells(
			const TSet<FIntVector>& Left,
			const TSet<FIntVector>& Right)
		{
			if (Left.Num() != Right.Num()) return false;
			for (const FIntVector& Cell : Left)
			{
				if (!Right.Contains(Cell)) return false;
			}
			return true;
		}

		int32 ExposedFaceCount(const TSet<FIntVector>& Cells)
		{
			static const FIntVector Directions[] = {
				FIntVector(1, 0, 0), FIntVector(-1, 0, 0),
				FIntVector(0, 1, 0), FIntVector(0, -1, 0),
				FIntVector(0, 0, 1), FIntVector(0, 0, -1),
			};
			int32 Result = 0;
			for (const FIntVector& Cell : Cells)
			{
				for (const FIntVector& Direction : Directions)
				{
					Result += Cells.Contains(Cell + Direction) ? 0 : 1;
				}
			}
			return Result;
		}

		FVector2f CubeGridUv(
			const FVector3d& LocalPosition,
			const FVector& AnchorLocal,
			const FVector3d& FaceNormal)
		{
			const FVector3d GridPosition =
				LocalPosition + FVector3d(AnchorLocal);
			const FVector3d AbsNormal(
				FMath::Abs(FaceNormal.X),
				FMath::Abs(FaceNormal.Y),
				FMath::Abs(FaceNormal.Z));
			FVector2d Uv;
			if (AbsNormal.X >= AbsNormal.Y && AbsNormal.X >= AbsNormal.Z)
			{
				Uv = FVector2d(GridPosition.Y, GridPosition.Z);
			}
			else if (AbsNormal.Y >= AbsNormal.X && AbsNormal.Y >= AbsNormal.Z)
			{
				Uv = FVector2d(GridPosition.X, GridPosition.Z);
			}
			else
			{
				Uv = FVector2d(GridPosition.X, GridPosition.Y);
			}
			constexpr double WorldSpaceUvScale = 0.01;
			return FVector2f(
				static_cast<float>(Uv.X * WorldSpaceUvScale),
				static_cast<float>(1.0 - Uv.Y * WorldSpaceUvScale));
		}

		void AppendQuad(
			FDynamicMesh3& Mesh,
			const FVector& A,
			const FVector& B,
			const FVector& C,
			const FVector& D,
			const FVector& InsidePoint,
			const FVector& AnchorLocal)
		{
			FVector3d V0(A);
			FVector3d V1(B);
			FVector3d V2(C);
			FVector3d V3(D);
			const FVector3d Inside(InsidePoint);
			FVector3d Cross = (V1 - V0).Cross(V2 - V0);
			const FVector3d Center = (V0 + V1 + V2 + V3) / 4.0;
			if (Cross.Dot(Center - Inside) > 0.0)
			{
				Swap(V1, V3);
				Cross = -Cross;
			}
			FVector3d Normal = -Cross;
			const double Length = Normal.Length();
			Normal = Length > UE_DOUBLE_SMALL_NUMBER
				? Normal / Length : FVector3d::UnitZ();

			const int32 Vertex0 = Mesh.AppendVertex(V0);
			const int32 Vertex1 = Mesh.AppendVertex(V1);
			const int32 Vertex2 = Mesh.AppendVertex(V2);
			const int32 Vertex3 = Mesh.AppendVertex(V3);
			const int32 Triangle0 = Mesh.AppendTriangle(Vertex0, Vertex1, Vertex2);
			const int32 Triangle1 = Mesh.AppendTriangle(Vertex0, Vertex2, Vertex3);
			if (!Mesh.HasAttributes()) return;

			if (FDynamicMeshNormalOverlay* Overlay =
				Mesh.Attributes()->PrimaryNormals())
			{
				const FVector3f Value(
					static_cast<float>(Normal.X),
					static_cast<float>(Normal.Y),
					static_cast<float>(Normal.Z));
				const int32 A0 = Overlay->AppendElement(Value);
				const int32 A1 = Overlay->AppendElement(Value);
				const int32 A2 = Overlay->AppendElement(Value);
				const int32 A3 = Overlay->AppendElement(Value);
				Overlay->SetTriangle(Triangle0, FIndex3i(A0, A1, A2));
				Overlay->SetTriangle(Triangle1, FIndex3i(A0, A2, A3));
			}
			if (FDynamicMeshUVOverlay* Overlay = Mesh.Attributes()->PrimaryUV())
			{
				const int32 A0 = Overlay->AppendElement(
					CubeGridUv(V0, AnchorLocal, Normal));
				const int32 A1 = Overlay->AppendElement(
					CubeGridUv(V1, AnchorLocal, Normal));
				const int32 A2 = Overlay->AppendElement(
					CubeGridUv(V2, AnchorLocal, Normal));
				const int32 A3 = Overlay->AppendElement(
					CubeGridUv(V3, AnchorLocal, Normal));
				Overlay->SetTriangle(Triangle0, FIndex3i(A0, A1, A2));
				Overlay->SetTriangle(Triangle1, FIndex3i(A0, A2, A3));
			}
			if (FDynamicMeshMaterialAttribute* MaterialIds =
				Mesh.Attributes()->GetMaterialID())
			{
				MaterialIds->SetValue(Triangle0, 0);
				MaterialIds->SetValue(Triangle1, 0);
			}
		}

		int32 BuildExposedFaceMesh(
			const TSet<FIntVector>& Cells,
			const double CellSize,
			const FVector& AnchorLocal,
			FDynamicMesh3& OutMesh)
		{
			OutMesh.Clear();
			OutMesh.EnableAttributes();
			OutMesh.Attributes()->EnableMaterialID();
			int32 FaceCount = 0;
			for (const FIntVector& Cell : SortedCells(Cells))
			{
				const FVector Min(
					Cell.X * CellSize,
					Cell.Y * CellSize,
					Cell.Z * CellSize);
				const FVector P0 = Min - AnchorLocal;
				const FVector P1 = P0 + FVector(CellSize);
				const FVector Inside = (P0 + P1) * 0.5;
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
					AppendQuad(OutMesh, C100, C110, C111, C101, Inside, AnchorLocal);
					++FaceCount;
				}
				if (!Cells.Contains(Cell + FIntVector(-1, 0, 0)))
				{
					AppendQuad(OutMesh, C010, C000, C001, C011, Inside, AnchorLocal);
					++FaceCount;
				}
				if (!Cells.Contains(Cell + FIntVector(0, 1, 0)))
				{
					AppendQuad(OutMesh, C110, C010, C011, C111, Inside, AnchorLocal);
					++FaceCount;
				}
				if (!Cells.Contains(Cell + FIntVector(0, -1, 0)))
				{
					AppendQuad(OutMesh, C000, C100, C101, C001, Inside, AnchorLocal);
					++FaceCount;
				}
				if (!Cells.Contains(Cell + FIntVector(0, 0, 1)))
				{
					AppendQuad(OutMesh, C001, C101, C111, C011, Inside, AnchorLocal);
					++FaceCount;
				}
				if (!Cells.Contains(Cell + FIntVector(0, 0, -1)))
				{
					AppendQuad(OutMesh, C100, C000, C010, C110, Inside, AnchorLocal);
					++FaceCount;
				}
			}
			return FaceCount;
		}

		bool SerializeSidecar(
			const FSession& Session,
			FString& OutText,
			FString& OutError)
		{
			OutText.Reset();
			const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("schema"), SidecarSchema);
			Root->SetStringField(TEXT("grid_id"), Session.GridId);
			Root->SetNumberField(TEXT("cell_size"), Session.CellSize);
			Root->SetObjectField(
				TEXT("grid_origin"), VectorJson(Session.GridToWorld.GetLocation()));
			Root->SetObjectField(
				TEXT("grid_rotation"), RotatorJson(Session.GridToWorld.Rotator()));
			Root->SetObjectField(TEXT("anchor"), VectorJson(Session.AnchorLocal));
			Root->SetNumberField(TEXT("revision"), Session.Revision);
			Root->SetStringField(
				TEXT("collision_generation"), Session.CollisionGeneration);
			Root->SetStringField(TEXT("collision_mode"), Session.CollisionMode);
			if (const ADynamicMeshActor* Actor = Session.Actor.Get())
			{
				Root->SetStringField(TEXT("actor_path"), Actor->GetPathName());
			}
			TArray<TSharedPtr<FJsonValue>> Cells;
			Cells.Reserve(Session.Cells.Num());
			for (const FIntVector& Cell : SortedCells(Session.Cells))
			{
				Cells.Add(MakeShared<FJsonValueObject>(IntVectorJson(Cell)));
			}
			Root->SetArrayField(TEXT("cells"), MoveTemp(Cells));
			const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>>
				Writer = TJsonWriterFactory<TCHAR,
					TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutText);
			if (!FJsonSerializer::Serialize(Root, Writer) || OutText.IsEmpty())
			{
				OutError = TEXT("voxel_grid_sidecar_serialize_failed");
				return false;
			}
			return true;
		}

		bool CommitTextFile(
			const FString& Path,
			const FString& Text,
			FString& OutError)
		{
			const FString Directory = FPaths::GetPath(Path);
			if (!IFileManager::Get().MakeDirectory(*Directory, true)
				&& !IFileManager::Get().DirectoryExists(*Directory))
			{
				OutError = TEXT("voxel_grid_sidecar_directory_failed");
				return false;
			}
			const FString Temporary = Path + TEXT(".tmp.")
				+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
			if (!FFileHelper::SaveStringToFile(
				Text,
				*Temporary,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				OutError = TEXT("voxel_grid_sidecar_stage_failed");
				return false;
			}
			if (!IFileManager::Get().Move(
				*Path, *Temporary, true, true, false, true))
			{
				IFileManager::Get().Delete(*Temporary, false, true, true);
				OutError = TEXT("voxel_grid_sidecar_commit_failed");
				return false;
			}
			return true;
		}

		bool RestoreSidecar(
			const FString& Path,
			const bool bHadPrevious,
			const FString& Previous)
		{
			if (bHadPrevious)
			{
				FString Ignored;
				return CommitTextFile(Path, Previous, Ignored);
			}
			return !IFileManager::Get().FileExists(*Path)
				|| IFileManager::Get().Delete(*Path, false, true, true);
		}

		bool TryReadCellValue(
			const TSharedPtr<FJsonValue>& Value,
			FIntVector& OutCell)
		{
			const TSharedPtr<FJsonObject> Object =
				Value.IsValid() ? Value->AsObject() : nullptr;
			int64 X = 0;
			int64 Y = 0;
			int64 Z = 0;
			if (!TryReadIntegralField(Object, TEXT("x"), X)
				|| !TryReadIntegralField(Object, TEXT("y"), Y)
				|| !TryReadIntegralField(Object, TEXT("z"), Z)
				|| X <= MIN_int32 || X >= MAX_int32
				|| Y <= MIN_int32 || Y >= MAX_int32
				|| Z <= MIN_int32 || Z >= MAX_int32)
			{
				return false;
			}
			OutCell = FIntVector(
				static_cast<int32>(X),
				static_cast<int32>(Y),
				static_cast<int32>(Z));
			return true;
		}

		bool LoadSidecar(
			FSession& Session,
			FString& OutError)
		{
			FString Text;
			if (!FFileHelper::LoadFileToString(Text, *Session.SidecarPath))
			{
				OutError = TEXT("voxel_grid_sidecar_missing");
				return false;
			}
			TSharedPtr<FJsonObject> Root;
			if (!FJsonSerializer::Deserialize(
				TJsonReaderFactory<>::Create(Text), Root)
				|| !Root.IsValid())
			{
				OutError = TEXT("voxel_grid_sidecar_parse_failed");
				return false;
			}
			FString Schema;
			FString GridId;
			double CellSize = 0.0;
			int64 Revision = 0;
			FVector Origin = FVector::ZeroVector;
			FRotator Rotation = FRotator::ZeroRotator;
			FVector Anchor = FVector::ZeroVector;
			if (!Root->TryGetStringField(TEXT("schema"), Schema)
				|| (Schema != SidecarSchema && Schema != LegacySidecarSchema)
				|| !Root->TryGetStringField(TEXT("grid_id"), GridId)
				|| GridId != Session.GridId
				|| !Root->TryGetNumberField(TEXT("cell_size"), CellSize)
				|| !FMath::IsFinite(CellSize)
				|| CellSize <= KINDA_SMALL_NUMBER
				|| CellSize > 1000000.0
				|| !TryReadVector(Root, TEXT("grid_origin"), Origin)
				|| !TryReadRotator(Root, TEXT("grid_rotation"), Rotation)
				|| !TryReadIntegralField(Root, TEXT("revision"), Revision)
				|| Revision < 0)
			{
				OutError = TEXT("voxel_grid_sidecar_contract_invalid");
				return false;
			}
			TryReadVector(Root, TEXT("anchor"), Anchor);
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Root->TryGetArrayField(TEXT("cells"), Values)
				|| !Values || Values->Num() > MaxSessionCells)
			{
				OutError = TEXT("voxel_grid_sidecar_cells_invalid");
				return false;
			}
			TSet<FIntVector> Cells;
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				FIntVector Cell;
				if (!TryReadCellValue(Value, Cell))
				{
					OutError = TEXT("voxel_grid_sidecar_cell_invalid");
					return false;
				}
				Cells.Add(Cell);
			}
			if (Cells.Num() != Values->Num())
			{
				OutError = TEXT("voxel_grid_sidecar_cell_duplicate");
				return false;
			}
			Session.CellSize = CellSize;
			Session.GridToWorld = FTransform(Rotation, Origin, FVector::OneVector);
			Session.AnchorLocal = Anchor;
			Session.Cells = MoveTemp(Cells);
			Session.Revision = Revision;
			Session.bDirty = false;
			Session.bCommitted = true;
			Root->TryGetStringField(
				TEXT("collision_generation"), Session.CollisionGeneration);
			Root->TryGetStringField(TEXT("collision_mode"), Session.CollisionMode);
			return true;
		}

		UWorld* EditorWorld(FFailure& OutFailure)
		{
			if (!IsInGameThread())
			{
				SetFailure(
					OutFailure, TEXT("voxel_grid_game_thread_required"));
				return nullptr;
			}
			UWorld* const World = GEditor
				? GEditor->GetEditorWorldContext().World() : nullptr;
			if (!World)
			{
				SetFailure(
					OutFailure, TEXT("voxel_grid_editor_world_unavailable"));
			}
			return World;
		}

		bool TryGridIdFromActor(const AActor& Actor, FString& OutGridId)
		{
			for (const FName& Tag : Actor.Tags)
			{
				const FString Value = Tag.ToString();
				if (Value.StartsWith(ActorTagPrefix))
				{
					OutGridId = Value.RightChop(
						FString(ActorTagPrefix).Len());
					return IsSafeIdentity(OutGridId);
				}
			}
			return false;
		}

		ADynamicMeshActor* ResolveActor(
			UWorld& World,
			const FString& ActorRef,
			const FString& GridId,
			FString& OutError)
		{
			TArray<ADynamicMeshActor*> Matches;
			for (TActorIterator<ADynamicMeshActor> It(&World); It; ++It)
			{
				ADynamicMeshActor* const Actor = *It;
				bool bMatch = false;
				if (!ActorRef.IsEmpty())
				{
					bMatch = Actor->GetPathName() == ActorRef
						|| Actor->GetName() == ActorRef
						|| Actor->GetActorLabel() == ActorRef;
				}
				else if (!GridId.IsEmpty())
				{
					bMatch = Actor->ActorHasTag(FName(*ActorTag(GridId)));
				}
				if (bMatch) Matches.Add(Actor);
			}
			if (Matches.Num() != 1)
			{
				OutError = Matches.IsEmpty()
					? FString(TEXT("voxel_grid_actor_not_found"))
					: FString(TEXT("voxel_grid_actor_ambiguous"));
				return nullptr;
			}
			return Matches[0];
		}

		bool HasActorForGrid(UWorld& World, const FString& GridId)
		{
			for (TActorIterator<ADynamicMeshActor> It(&World); It; ++It)
			{
				if ((*It)->ActorHasTag(FName(*ActorTag(GridId)))) return true;
			}
			return false;
		}

		UDynamicMeshComponent* DynamicMeshComponent(ADynamicMeshActor* Actor)
		{
			return Actor
				? Actor->FindComponentByClass<UDynamicMeshComponent>() : nullptr;
		}

		bool ParseBounds(
			const TSharedPtr<FJsonObject>& Params,
			const FSession& Session,
			FVoxelGridCellRange& OutRange,
			TSharedPtr<FJsonObject>& OutReadback,
			FString& OutError)
		{
			const TSharedPtr<FJsonObject>* BoundsObject = nullptr;
			if (!Params.IsValid()
				|| !Params->TryGetObjectField(TEXT("bounds"), BoundsObject)
				|| !BoundsObject || !BoundsObject->IsValid())
			{
				OutError = TEXT("voxel_grid_bounds_required");
				return false;
			}
			FVector Min = FVector::ZeroVector;
			FVector Max = FVector::ZeroVector;
			if (!TryReadVector(*BoundsObject, TEXT("min"), Min)
				|| !TryReadVector(*BoundsObject, TEXT("max"), Max))
			{
				FVector Center = FVector::ZeroVector;
				FVector Extent = FVector::ZeroVector;
				if (!TryReadVector(*BoundsObject, TEXT("center"), Center)
					|| !TryReadVector(*BoundsObject, TEXT("extent"), Extent)
					|| Extent.GetMin() <= 0.0)
				{
					OutError = TEXT("voxel_grid_bounds_shape_invalid");
					return false;
				}
				Min = Center - Extent;
				Max = Center + Extent;
			}
			const FVector NormalizedMin(
				FMath::Min(Min.X, Max.X),
				FMath::Min(Min.Y, Max.Y),
				FMath::Min(Min.Z, Max.Z));
			const FVector NormalizedMax(
				FMath::Max(Min.X, Max.X),
				FMath::Max(Min.Y, Max.Y),
				FMath::Max(Min.Z, Max.Z));
			FString Space;
			if (!(*BoundsObject)->TryGetStringField(TEXT("space"), Space))
			{
				OutError = TEXT("voxel_grid_bounds_space_required");
				return false;
			}
			Space.TrimStartAndEndInline();
			Space.ToLowerInline();
			if (!TryConvertBoundsToCellRange(
				FBox(NormalizedMin, NormalizedMax),
				Space,
				Session.GridToWorld,
				Session.AnchorLocal,
				Session.CellSize,
				OutRange,
				OutError))
			{
				return false;
			}
			int64 RequestedLimit = MaxOperationCells;
			if (Params->HasField(TEXT("max_operation_cells"))
				&& (!TryReadIntegralField(
					Params, TEXT("max_operation_cells"), RequestedLimit)
					|| RequestedLimit <= 0))
			{
				OutError = TEXT("voxel_grid_operation_limit_invalid");
				return false;
			}
			if (OutRange.NumCells()
				> FMath::Min<int64>(RequestedLimit, MaxOperationCells))
			{
				OutError = TEXT("voxel_grid_operation_cell_limit_exceeded");
				return false;
			}
			OutReadback = MakeShared<FJsonObject>();
			OutReadback->SetStringField(TEXT("input_space"), Space);
			OutReadback->SetObjectField(TEXT("input_min"), VectorJson(NormalizedMin));
			OutReadback->SetObjectField(TEXT("input_max"), VectorJson(NormalizedMax));
			OutReadback->SetObjectField(TEXT("cell_range"), RangeJson(OutRange));
			OutReadback->SetStringField(
				TEXT("selection_semantics"), TEXT("conservative_aabb_cover"));
			return true;
		}

		bool CheckRevision(
			const TSharedPtr<FJsonObject>& Params,
			const FSession& Session,
			FString& OutError,
			const bool bRequired = true)
		{
			int64 Expected = 0;
			if (!TryReadIntegralField(Params, TEXT("expected_revision"), Expected))
			{
				if (!bRequired && Params.IsValid()
					&& !Params->HasField(TEXT("expected_revision")))
				{
					return true;
				}
				OutError = TEXT("voxel_grid_expected_revision_required");
				return false;
			}
			if (Expected != Session.Revision)
			{
				OutError = TEXT("voxel_grid_revision_mismatch");
				return false;
			}
			return true;
		}

		TSharedRef<FJsonObject> SessionObservation(
			const FSession& Session,
			const TSharedPtr<FJsonObject>& Params)
		{
			const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("session_id"), Session.SessionId);
			Result->SetStringField(TEXT("grid_id"), Session.GridId);
			Result->SetNumberField(TEXT("revision"), Session.Revision);
			Result->SetNumberField(TEXT("cell_size"), Session.CellSize);
			Result->SetNumberField(TEXT("occupied_cell_count"), Session.Cells.Num());
			Result->SetBoolField(TEXT("dirty"), Session.bDirty);
			Result->SetBoolField(TEXT("committed"), Session.bCommitted);
			Result->SetObjectField(
				TEXT("grid_origin"), VectorJson(Session.GridToWorld.GetLocation()));
			Result->SetObjectField(
				TEXT("grid_rotation"), RotatorJson(Session.GridToWorld.Rotator()));
			Result->SetObjectField(TEXT("anchor"), VectorJson(Session.AnchorLocal));
			Result->SetStringField(TEXT("sidecar_path"), Session.SidecarPath);
			Result->SetStringField(
				TEXT("collision_generation"), Session.CollisionGeneration);
			Result->SetStringField(TEXT("collision_mode"), Session.CollisionMode);
			if (const ADynamicMeshActor* Actor = Session.Actor.Get())
			{
				Result->SetStringField(TEXT("actor_ref"), Actor->GetPathName());
				Result->SetStringField(TEXT("actor_name"), Actor->GetName());
				Result->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
			}

			const TSharedPtr<FJsonObject>* Projection = nullptr;
			bool bIncludeCells = false;
			int64 MaxCells = 0;
			if (Params.IsValid()
				&& Params->TryGetObjectField(TEXT("projection"), Projection)
				&& Projection && Projection->IsValid())
			{
				(*Projection)->TryGetBoolField(TEXT("include_cells"), bIncludeCells);
				TryReadIntegralField(*Projection, TEXT("max_cells"), MaxCells);
			}
			if (bIncludeCells && MaxCells > 0)
			{
				TArray<TSharedPtr<FJsonValue>> Values;
				for (const FIntVector& Cell : SortedCells(Session.Cells))
				{
					if (Values.Num() >= FMath::Min<int64>(MaxCells, 256)) break;
					Values.Add(MakeShared<FJsonValueObject>(IntVectorJson(Cell)));
				}
				Result->SetArrayField(TEXT("cells"), MoveTemp(Values));
				Result->SetBoolField(
					TEXT("cells_complete"), MaxCells >= Session.Cells.Num());
			}
			return Result;
		}

		bool AreCellsPresent(
			const TSet<FIntVector>& Cells,
			const FIntVector& Min,
			const FIntVector& MaxExclusive)
		{
			for (int32 Z = Min.Z; Z < MaxExclusive.Z; ++Z)
			{
				for (int32 Y = Min.Y; Y < MaxExclusive.Y; ++Y)
				{
					for (int32 X = Min.X; X < MaxExclusive.X; ++X)
					{
						if (!Cells.Contains(FIntVector(X, Y, Z))) return false;
					}
				}
			}
			return true;
		}

		bool ComponentHasTag(const UActorComponent& Component, const FString& Tag)
		{
			return Component.ComponentHasTag(FName(*Tag));
		}

		void DestroyComponents(const TArray<UBoxComponent*>& Components)
		{
			for (UBoxComponent* Component : Components)
			{
				if (IsValid(Component)) Component->DestroyComponent();
			}
		}

		void RemoveOldCollisionComponents(
			ADynamicMeshActor& Actor,
			const FString& GridId,
			const TArray<UBoxComponent*>& Keep)
		{
			TSet<const UBoxComponent*> Kept;
			for (const UBoxComponent* Component : Keep) Kept.Add(Component);
			TArray<UActorComponent*> Components;
			Actor.GetComponents(UBoxComponent::StaticClass(), Components);
			TArray<UBoxComponent*> Remove;
			for (UActorComponent* Component : Components)
			{
				UBoxComponent* const Box = Cast<UBoxComponent>(Component);
				if (Box && !Kept.Contains(Box)
					&& ComponentHasTag(*Box, CollisionTag(GridId)))
				{
					Remove.Add(Box);
				}
			}
			DestroyComponents(Remove);
		}

		bool PrepareCollisionComponents(
			ADynamicMeshActor& Actor,
			const FSession& Session,
			const TArray<FVoxelGridCollisionBox>& Boxes,
			const FString& Generation,
			const FName& Profile,
			const ECollisionEnabled::Type Enabled,
			TArray<UBoxComponent*>& OutComponents,
			FString& OutError)
		{
			OutComponents.Reset();
			USceneComponent* const Root = Actor.GetRootComponent();
			if (!Root)
			{
				OutError = TEXT("voxel_grid_actor_root_missing");
				return false;
			}
			for (int32 Index = 0; Index < Boxes.Num(); ++Index)
			{
				const FVoxelGridCollisionBox& Box = Boxes[Index];
				const FName Name = MakeUniqueObjectName(
					&Actor,
					UBoxComponent::StaticClass(),
					FName(*FString::Printf(TEXT("UAI_VoxelCollision_%03d"), Index)));
				UBoxComponent* const Component = NewObject<UBoxComponent>(
					&Actor, Name, RF_Transactional);
				if (!Component)
				{
					OutError = TEXT("voxel_grid_collision_component_create_failed");
					return false;
				}
				const FVector GridMin(
					Box.Min.X * Session.CellSize,
					Box.Min.Y * Session.CellSize,
					Box.Min.Z * Session.CellSize);
				const FVector GridMax(
					Box.MaxExclusive.X * Session.CellSize,
					Box.MaxExclusive.Y * Session.CellSize,
					Box.MaxExclusive.Z * Session.CellSize);
				Component->CreationMethod = EComponentCreationMethod::Instance;
				Component->SetMobility(EComponentMobility::Movable);
				Component->SetupAttachment(Root);
				Component->SetRelativeLocation(
					(GridMin + GridMax) * 0.5 - Session.AnchorLocal);
				Component->SetRelativeRotation(FRotator::ZeroRotator);
				Component->SetRelativeScale3D(FVector::OneVector);
				Component->SetBoxExtent((GridMax - GridMin) * 0.5, false);
				Component->SetCollisionProfileName(Profile);
				Component->SetCollisionEnabled(Enabled);
				Component->SetHiddenInGame(true);
				Component->SetVisibility(false, true);
				Component->ComponentTags.AddUnique(FName(*CollisionTag(Session.GridId)));
				Component->ComponentTags.AddUnique(FName(*CollisionGenerationTag(
					Session.GridId, Generation)));
				OutComponents.Add(Component);
			}
			return true;
		}

		bool ActivateCollisionComponents(
			ADynamicMeshActor& Actor,
			const TArray<UBoxComponent*>& Components,
			FString& OutError)
		{
			for (UBoxComponent* Component : Components)
			{
				Actor.AddInstanceComponent(Component);
				Component->RegisterComponent();
				Component->UpdateOverlaps();
				if (!Component->IsRegistered())
				{
					OutError = TEXT("voxel_grid_collision_component_register_failed");
					return false;
				}
			}
			return true;
		}

		bool TryRangeFromComponent(
			const UBoxComponent& Component,
			const FSession& Session,
			FVoxelGridCollisionBox& OutRange)
		{
			if (!Component.GetRelativeRotation().IsNearlyZero(0.001)
				|| !Component.GetRelativeScale3D().Equals(FVector::OneVector, 0.001))
			{
				return false;
			}
			const FVector Center =
				Component.GetRelativeLocation() + Session.AnchorLocal;
			const FVector Extent = Component.GetUnscaledBoxExtent();
			const FVector RawMin = (Center - Extent) / Session.CellSize;
			const FVector RawMax = (Center + Extent) / Session.CellSize;
			const FVector RoundedMin(
				FMath::RoundToDouble(RawMin.X),
				FMath::RoundToDouble(RawMin.Y),
				FMath::RoundToDouble(RawMin.Z));
			const FVector RoundedMax(
				FMath::RoundToDouble(RawMax.X),
				FMath::RoundToDouble(RawMax.Y),
				FMath::RoundToDouble(RawMax.Z));
			if (!RawMin.Equals(RoundedMin, 0.001)
				|| !RawMax.Equals(RoundedMax, 0.001))
			{
				return false;
			}
			OutRange.Min = FIntVector(
				static_cast<int32>(RoundedMin.X),
				static_cast<int32>(RoundedMin.Y),
				static_cast<int32>(RoundedMin.Z));
			OutRange.MaxExclusive = FIntVector(
				static_cast<int32>(RoundedMax.X),
				static_cast<int32>(RoundedMax.Y),
				static_cast<int32>(RoundedMax.Z));
			return OutRange.IsValid();
		}

		bool VerifyAppliedState(
			const FSession& Expected,
			const FString& MaterialPath,
			TSharedRef<FJsonObject>& OutReadback,
			FString& OutError)
		{
			OutReadback = MakeShared<FJsonObject>();
			ADynamicMeshActor* const Actor = Expected.Actor.Get();
			UDynamicMeshComponent* const Component = DynamicMeshComponent(Actor);
			if (!Actor || !Component || !Component->GetDynamicMesh())
			{
				OutError = TEXT("voxel_grid_readback_actor_missing");
				return false;
			}
			FSession Persisted = Expected;
			Persisted.Cells.Reset();
			if (!LoadSidecar(Persisted, OutError)) return false;
			const bool bSidecarEquivalent =
				Persisted.GridId == Expected.GridId
				&& Persisted.Revision == Expected.Revision
				&& FMath::IsNearlyEqual(
					Persisted.CellSize, Expected.CellSize, 1.e-6)
				&& Persisted.GridToWorld.Equals(Expected.GridToWorld, 0.001)
				&& Persisted.AnchorLocal.Equals(Expected.AnchorLocal, 0.001)
				&& Persisted.CollisionGeneration == Expected.CollisionGeneration
				&& Persisted.CollisionMode == Expected.CollisionMode
				&& SameCells(Persisted.Cells, Expected.Cells);

			int32 VertexCount = 0;
			int32 TriangleCount = 0;
			Component->GetDynamicMesh()->ProcessMesh(
				[&](const FDynamicMesh3& Mesh)
				{
					VertexCount = Mesh.VertexCount();
					TriangleCount = Mesh.TriangleCount();
				});
			const int32 FaceCount = ExposedFaceCount(Expected.Cells);
			const bool bMeshEquivalent =
				VertexCount == FaceCount * 4
				&& TriangleCount == FaceCount * 2;

			TSet<FIntVector> Covered;
			bool bCollisionShapeValid = true;
			int32 CollisionBoxCount = 0;
			TArray<UActorComponent*> Components;
			Actor->GetComponents(UBoxComponent::StaticClass(), Components);
			const FString GenerationTag = CollisionGenerationTag(
				Expected.GridId, Expected.CollisionGeneration);
			for (UActorComponent* Item : Components)
			{
				UBoxComponent* const Box = Cast<UBoxComponent>(Item);
				if (!Box || !ComponentHasTag(*Box, GenerationTag)) continue;
				++CollisionBoxCount;
				FVoxelGridCollisionBox Range;
				if (!TryRangeFromComponent(*Box, Expected, Range))
				{
					bCollisionShapeValid = false;
					continue;
				}
				for (int32 Z = Range.Min.Z; Z < Range.MaxExclusive.Z; ++Z)
				{
					for (int32 Y = Range.Min.Y; Y < Range.MaxExclusive.Y; ++Y)
					{
						for (int32 X = Range.Min.X; X < Range.MaxExclusive.X; ++X)
						{
							const FIntVector Cell(X, Y, Z);
							if (Covered.Contains(Cell)) bCollisionShapeValid = false;
							Covered.Add(Cell);
						}
					}
				}
			}
			const bool bCollisionEquivalent = Expected.CollisionMode == TEXT("none")
				? CollisionBoxCount == 0
				: bCollisionShapeValid && SameCells(Covered, Expected.Cells);
			const UMaterialInterface* const Material = Component->GetMaterial(0);
			const FString ObservedMaterial = Material ? Material->GetPathName() : FString();
			const bool bMaterialEquivalent = ObservedMaterial == MaterialPath;
			const bool bTransformEquivalent =
				Actor->GetActorTransform().Equals(ActorTransform(Expected), 0.001);

			OutReadback->SetBoolField(
				TEXT("sidecar_equivalent"), bSidecarEquivalent);
			OutReadback->SetBoolField(TEXT("mesh_equivalent"), bMeshEquivalent);
			OutReadback->SetBoolField(
				TEXT("collision_exact_cover"), bCollisionEquivalent);
			OutReadback->SetBoolField(
				TEXT("material_equivalent"), bMaterialEquivalent);
			OutReadback->SetBoolField(
				TEXT("transform_equivalent"), bTransformEquivalent);
			OutReadback->SetNumberField(TEXT("vertex_count"), VertexCount);
			OutReadback->SetNumberField(TEXT("triangle_count"), TriangleCount);
			OutReadback->SetNumberField(TEXT("exposed_face_count"), FaceCount);
			OutReadback->SetNumberField(
				TEXT("collision_box_count"), CollisionBoxCount);
			OutReadback->SetNumberField(
				TEXT("covered_cell_count"), Covered.Num());
			const bool bEquivalent = bSidecarEquivalent
				&& bMeshEquivalent
				&& bCollisionEquivalent
				&& bMaterialEquivalent
				&& bTransformEquivalent;
			OutReadback->SetBoolField(TEXT("equivalent"), bEquivalent);
			if (!bEquivalent)
			{
				OutError = TEXT("voxel_grid_independent_readback_mismatch");
			}
			return bEquivalent;
		}

		class FProcessRegistry final
		{
		public:
			bool ExecuteSession(
				const FUeAgentCommandRequest& Request,
				TSharedPtr<FJsonObject>& OutPayload,
				FFailure& OutFailure)
			{
				OutPayload.Reset();
				OutFailure = {};
				FScopeLock Lock(&Mutex);
				UWorld* const World = EditorWorld(OutFailure);
				if (!World) return false;
				FString Action;
				const TSharedPtr<FJsonObject>& Params = Request.GetParams();
				if (!Params.IsValid()
					|| !Params->TryGetStringField(TEXT("action"), Action))
				{
					SetFailure(OutFailure, TEXT("voxel_grid_action_required"));
					return false;
				}
				Action.TrimStartAndEndInline();
				Action.ToLowerInline();
				if (Action == TEXT("begin"))
				{
					return Begin(Request, *World, OutPayload, OutFailure);
				}
				FString SessionId;
				if (!Params->TryGetStringField(TEXT("session_id"), SessionId))
				{
					SetFailure(OutFailure, TEXT("voxel_grid_session_id_required"));
					return false;
				}
				FSession* const Session = Sessions.Find(SessionId);
				if (!Session || !Session->Actor.IsValid())
				{
					SetFailure(OutFailure, TEXT("voxel_grid_session_not_found"));
					return false;
				}
				if (Action == TEXT("read"))
				{
					FString Error;
					if (!CheckRevision(Params, *Session, Error, false))
					{
						SetFailure(OutFailure, Error);
						return false;
					}
					OutPayload = SuccessPayload(
						Request, Action, SessionObservation(*Session, Params));
					return true;
				}
				if (Action == TEXT("add_bounds")
					|| Action == TEXT("remove_bounds"))
				{
					return MutateBounds(
						Request, Action, *Session, OutPayload, OutFailure);
				}
				if (Action == TEXT("close") || Action == TEXT("cancel"))
				{
					return Close(
						Request, Action, *Session, OutPayload, OutFailure);
				}
				SetFailure(OutFailure, TEXT("voxel_grid_action_unknown"), Action);
				return false;
			}

			bool ExecuteApply(
				const FUeAgentCommandRequest& Request,
				TSharedPtr<FJsonObject>& OutPayload,
				FFailure& OutFailure)
			{
				OutPayload.Reset();
				OutFailure = {};
				FScopeLock Lock(&Mutex);
				if (!EditorWorld(OutFailure)) return false;
				const TSharedPtr<FJsonObject>& Params = Request.GetParams();
				FString SessionId;
				if (!Params.IsValid()
					|| !Params->TryGetStringField(TEXT("session_id"), SessionId))
				{
					SetFailure(OutFailure, TEXT("voxel_grid_session_id_required"));
					return false;
				}
				FSession* const Session = Sessions.Find(SessionId);
				FString Error;
				if (!Session || !Session->Actor.IsValid())
				{
					SetFailure(OutFailure, TEXT("voxel_grid_session_not_found"));
					return false;
				}
				if (!CheckRevision(Params, *Session, Error))
				{
					SetFailure(OutFailure, Error);
					return false;
				}
				ADynamicMeshActor* const Actor = Session->Actor.Get();
				UDynamicMeshComponent* const Component = DynamicMeshComponent(Actor);
				if (!Actor || !Component || !Component->GetDynamicMesh())
				{
					SetFailure(
						OutFailure, TEXT("voxel_grid_dynamic_mesh_component_required"));
					return false;
				}

				FString MeshBuild = TEXT("exposed_faces");
				Params->TryGetStringField(TEXT("mesh_build"), MeshBuild);
				MeshBuild.ToLowerInline();
				if (MeshBuild != TEXT("exposed_faces"))
				{
					SetFailure(
						OutFailure, TEXT("voxel_grid_mesh_build_unsupported"), MeshBuild);
					return false;
				}

				FString CollisionMode = TEXT("exact_box_components");
				FString CollisionProfile = TEXT("BlockAll");
				FString CollisionEnabled = TEXT("query_and_physics");
				int64 CollisionLimit = MaxCollisionBoxes;
				const TSharedPtr<FJsonObject>* Collision = nullptr;
				if (Params->TryGetObjectField(TEXT("collision"), Collision)
					&& Collision && Collision->IsValid())
				{
					(*Collision)->TryGetStringField(TEXT("mode"), CollisionMode);
					(*Collision)->TryGetStringField(
						TEXT("profile_name"), CollisionProfile);
					(*Collision)->TryGetStringField(
						TEXT("enabled"), CollisionEnabled);
					if ((*Collision)->HasField(TEXT("max_boxes"))
						&& (!TryReadIntegralField(
							*Collision, TEXT("max_boxes"), CollisionLimit)
							|| CollisionLimit < 0))
					{
						SetFailure(
							OutFailure, TEXT("voxel_grid_collision_limit_invalid"));
						return false;
					}
				}
				CollisionMode.TrimStartAndEndInline();
				CollisionMode.ToLowerInline();
				CollisionEnabled.TrimStartAndEndInline();
				CollisionEnabled.ToLowerInline();
				if (CollisionMode == TEXT("disabled")) CollisionMode = TEXT("none");
				if (CollisionMode != TEXT("none")
					&& CollisionMode != TEXT("exact_box_components"))
				{
					SetFailure(
						OutFailure, TEXT("voxel_grid_collision_mode_unsupported"),
						CollisionMode);
					return false;
				}
				if (CollisionEnabled != TEXT("none")
					&& CollisionEnabled != TEXT("no_collision")
					&& CollisionEnabled != TEXT("query_only")
					&& CollisionEnabled != TEXT("physics_only")
					&& CollisionEnabled != TEXT("query_and_physics"))
				{
					SetFailure(
						OutFailure,
						TEXT("voxel_grid_collision_enabled_unsupported"),
						CollisionEnabled);
					return false;
				}
				if (CollisionProfile.TrimStartAndEnd().IsEmpty())
				{
					SetFailure(
						OutFailure, TEXT("voxel_grid_collision_profile_invalid"));
					return false;
				}
				TArray<FVoxelGridCollisionBox> Boxes;
				if (CollisionMode != TEXT("none"))
				{
					Boxes = BuildExactCoverBoxes(Session->Cells);
					if (Boxes.Num() > FMath::Min<int64>(
						CollisionLimit, MaxCollisionBoxes))
					{
						SetFailure(
							OutFailure,
							TEXT("voxel_grid_collision_box_limit_exceeded"));
						return false;
					}
				}

				UMaterialInterface* Material = Component->GetMaterial(0);
				FString MaterialPath = Material ? Material->GetPathName() : FString();
				FString RequestedMaterial;
				if (Params->TryGetStringField(TEXT("material_ref"), RequestedMaterial)
					&& !RequestedMaterial.IsEmpty())
				{
					Material = LoadObject<UMaterialInterface>(nullptr, *RequestedMaterial);
					if (!Material)
					{
						SetFailure(
							OutFailure, TEXT("voxel_grid_material_not_found"),
							RequestedMaterial);
						return false;
					}
					MaterialPath = Material->GetPathName();
				}

				FDynamicMesh3 NewMesh;
				const int32 FaceCount = BuildExposedFaceMesh(
					Session->Cells, Session->CellSize, Session->AnchorLocal, NewMesh);
				FDynamicMesh3 OldMesh;
				Component->GetDynamicMesh()->ProcessMesh(
					[&](const FDynamicMesh3& Mesh) { OldMesh = Mesh; });
				UMaterialInterface* const OldMaterial = Component->GetMaterial(0);
				const ECollisionEnabled::Type OldCollisionEnabled =
					Component->GetCollisionEnabled();
				const FTransform OldActorTransform = Actor->GetActorTransform();

				const FString Generation = MakeGuid(TEXT("GEN_"));
				const ECollisionEnabled::Type Enabled =
					CollisionEnabled == TEXT("query_only")
						? ECollisionEnabled::QueryOnly
						: CollisionEnabled == TEXT("physics_only")
							? ECollisionEnabled::PhysicsOnly
							: (CollisionEnabled == TEXT("none")
								|| CollisionEnabled == TEXT("no_collision"))
								? ECollisionEnabled::NoCollision
								: ECollisionEnabled::QueryAndPhysics;
				TArray<UBoxComponent*> NewCollisionComponents;
				if (!PrepareCollisionComponents(
					*Actor,
					*Session,
					Boxes,
					Generation,
					FName(*CollisionProfile),
					Enabled,
					NewCollisionComponents,
					Error))
				{
					DestroyComponents(NewCollisionComponents);
					SetFailure(OutFailure, Error);
					return false;
				}

				FString PreviousSidecar;
				const bool bHadPreviousSidecar =
					FFileHelper::LoadFileToString(
						PreviousSidecar, *Session->SidecarPath);
				FSession Intended = *Session;
				Intended.CollisionGeneration = Generation;
				Intended.CollisionMode = CollisionMode;
				FString SidecarText;
				if (!SerializeSidecar(Intended, SidecarText, Error)
					|| !CommitTextFile(Session->SidecarPath, SidecarText, Error))
				{
					DestroyComponents(NewCollisionComponents);
					SetFailure(OutFailure, Error);
					return false;
				}

				if (!ActivateCollisionComponents(
					*Actor, NewCollisionComponents, Error))
				{
					DestroyComponents(NewCollisionComponents);
					const bool bRestored = RestoreSidecar(
						Session->SidecarPath,
						bHadPreviousSidecar,
						PreviousSidecar);
					SetFailure(
						OutFailure,
						Error,
						TEXT("collision activation failed"),
						bRestored ? FString(TEXT("rolled_back"))
							: FString(TEXT("state_unknown")),
						bRestored,
						true);
					return false;
				}

				Component->Modify();
				Component->SetMesh(MoveTemp(NewMesh));
				Component->SetMaterial(0, Material);
				Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
				Component->MarkRenderStateDirty();
				Actor->Modify();
				Actor->SetActorTransform(
					ActorTransform(*Session), false, nullptr,
					ETeleportType::TeleportPhysics);
				Actor->Tags.AddUnique(FName(*ActorTag(Session->GridId)));
				Intended.Actor = Actor;
				TSharedRef<FJsonObject> Readback = MakeShared<FJsonObject>();
				if (!VerifyAppliedState(Intended, MaterialPath, Readback, Error))
				{
					Component->SetMesh(MoveTemp(OldMesh));
					Component->SetMaterial(0, OldMaterial);
					Component->SetCollisionEnabled(OldCollisionEnabled);
					Component->MarkRenderStateDirty();
					Actor->SetActorTransform(
						OldActorTransform, false, nullptr,
						ETeleportType::TeleportPhysics);
					DestroyComponents(NewCollisionComponents);
					const bool bRestored = RestoreSidecar(
						Session->SidecarPath,
						bHadPreviousSidecar,
						PreviousSidecar);
					SetFailure(
						OutFailure,
						Error,
						TEXT("independent readback failed"),
						bRestored ? FString(TEXT("rolled_back"))
							: FString(TEXT("state_unknown")),
						bRestored,
						true);
					return false;
				}

				RemoveOldCollisionComponents(
					*Actor, Session->GridId, NewCollisionComponents);
				Actor->MarkPackageDirty();
				Session->CollisionGeneration = Generation;
				Session->CollisionMode = CollisionMode;
				Session->bDirty = false;
				Session->bCommitted = true;
				bool bKeepSession = false;
				Params->TryGetBoolField(TEXT("keep_session"), bKeepSession);

				const TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
				Payload->SetStringField(
					TEXT("operation_id"), TEXT("voxel-grid-apply:")
						+ Request.GetRequestId());
				Payload->SetStringField(TEXT("terminal_status"), TEXT("succeeded"));
				Payload->SetStringField(TEXT("session_id"), Session->SessionId);
				Payload->SetStringField(TEXT("grid_id"), Session->GridId);
				Payload->SetNumberField(TEXT("revision"), Session->Revision);
				Payload->SetNumberField(
					TEXT("occupied_cell_count"), Session->Cells.Num());
				Payload->SetBoolField(TEXT("session_closed"), !bKeepSession);
				const TSharedRef<FJsonObject> Mesh = MakeShared<FJsonObject>();
				Mesh->SetStringField(TEXT("build_mode"), MeshBuild);
				Mesh->SetNumberField(TEXT("exposed_face_count"), FaceCount);
				Mesh->SetNumberField(TEXT("vertex_count"), FaceCount * 4);
				Mesh->SetNumberField(TEXT("triangle_count"), FaceCount * 2);
				Mesh->SetStringField(TEXT("material_ref"), MaterialPath);
				Payload->SetObjectField(TEXT("mesh"), Mesh);
				const TSharedRef<FJsonObject> CollisionResult =
					MakeShared<FJsonObject>();
				CollisionResult->SetStringField(TEXT("mode"), CollisionMode);
				CollisionResult->SetNumberField(TEXT("box_count"), Boxes.Num());
				CollisionResult->SetNumberField(
					TEXT("covered_cell_count"), Session->Cells.Num());
				CollisionResult->SetBoolField(
					TEXT("exact_cover"), CollisionMode == TEXT("none")
						? Boxes.IsEmpty() : true);
				Payload->SetObjectField(TEXT("collision"), CollisionResult);
				Payload->SetStringField(TEXT("sidecar_path"), Session->SidecarPath);
				Payload->SetObjectField(TEXT("readback"), Readback);
				Payload->SetArrayField(TEXT("errors"), {});
				Payload->SetArrayField(TEXT("warnings"), {
					MakeShared<FJsonValueString>(
						TEXT("level_package_dirty_not_implicitly_saved"))});
				OutPayload = Payload;
				if (!bKeepSession) Sessions.Remove(SessionId);
				return true;
			}

#if WITH_DEV_AUTOMATION_TESTS
			void Reset()
			{
				FScopeLock Lock(&Mutex);
				for (TPair<FString, FSession>& Pair : Sessions)
				{
					FSession& Session = Pair.Value;
					if (Session.bCreatedActor && !Session.bCommitted)
					{
						if (ADynamicMeshActor* Actor = Session.Actor.Get())
						{
							if (UWorld* World = Actor->GetWorld()) World->DestroyActor(Actor);
						}
					}
				}
				Sessions.Reset();
			}
#endif

		private:
			TSharedPtr<FJsonObject> SuccessPayload(
				const FUeAgentCommandRequest& Request,
				const FString& Action,
				const TSharedRef<FJsonObject>& Readback) const
			{
				const TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
				Payload->SetStringField(
					TEXT("operation_id"), TEXT("voxel-grid-session:")
						+ Request.GetRequestId());
				Payload->SetStringField(TEXT("action"), Action);
				Payload->SetStringField(TEXT("terminal_status"), TEXT("succeeded"));
				Payload->SetObjectField(TEXT("readback"), Readback);
				Payload->SetArrayField(TEXT("errors"), {});
				Payload->SetArrayField(TEXT("warnings"), {});
				return Payload;
			}

			bool Begin(
				const FUeAgentCommandRequest& Request,
				UWorld& World,
				TSharedPtr<FJsonObject>& OutPayload,
				FFailure& OutFailure)
			{
				const TSharedPtr<FJsonObject>& Params = Request.GetParams();
				FString Mode = TEXT("create");
				Params->TryGetStringField(TEXT("mode"), Mode);
				Mode.TrimStartAndEndInline();
				Mode.ToLowerInline();
				if (Mode == TEXT("modify")) Mode = TEXT("edit");
				if (Mode != TEXT("create") && Mode != TEXT("edit"))
				{
					SetFailure(OutFailure, TEXT("voxel_grid_begin_mode_unsupported"), Mode);
					return false;
				}
				FString SessionId;
				Params->TryGetStringField(TEXT("session_id"), SessionId);
				if (SessionId.IsEmpty()) SessionId = MakeGuid(TEXT("VGS_"));
				if (!IsSafeIdentity(SessionId) || Sessions.Contains(SessionId))
				{
					SetFailure(OutFailure, TEXT("voxel_grid_session_id_conflict"));
					return false;
				}
				for (auto It = Sessions.CreateIterator(); It; ++It)
				{
					if (!It.Value().Actor.IsValid()) It.RemoveCurrent();
				}
				if (Sessions.Num() >= MaxSessions)
				{
					SetFailure(OutFailure, TEXT("voxel_grid_session_limit_exceeded"));
					return false;
				}

				FSession Session;
				Session.SessionId = SessionId;
				Params->TryGetStringField(TEXT("grid_id"), Session.GridId);
				FString ActorRef;
				Params->TryGetStringField(TEXT("actor_ref"), ActorRef);
				ADynamicMeshActor* Actor = nullptr;
				FString Error;
				if (Mode == TEXT("create"))
				{
					if (Session.GridId.IsEmpty()) Session.GridId = MakeGuid(TEXT("VG_"));
					if (!IsSafeIdentity(Session.GridId)
						|| IFileManager::Get().FileExists(*SidecarPath(Session.GridId))
						|| HasActorForGrid(World, Session.GridId))
					{
						SetFailure(OutFailure, TEXT("voxel_grid_grid_id_conflict"));
						return false;
					}
					double CellSize = 100.0;
					Params->TryGetNumberField(TEXT("cell_size"), CellSize);
					if (!FMath::IsFinite(CellSize)
						|| CellSize <= KINDA_SMALL_NUMBER || CellSize > 1000000.0)
					{
						SetFailure(OutFailure, TEXT("voxel_grid_cell_size_invalid"));
						return false;
					}
					FVector Origin = FVector::ZeroVector;
					FRotator Rotation = FRotator::ZeroRotator;
					FVector Anchor = FVector::ZeroVector;
					if (Params->HasField(TEXT("origin"))
						&& !TryReadVector(Params, TEXT("origin"), Origin))
					{
						SetFailure(OutFailure, TEXT("voxel_grid_origin_invalid"));
						return false;
					}
					if (Params->HasField(TEXT("rotation"))
						&& !TryReadRotator(Params, TEXT("rotation"), Rotation))
					{
						SetFailure(OutFailure, TEXT("voxel_grid_rotation_invalid"));
						return false;
					}
					if (Params->HasField(TEXT("anchor"))
						&& !TryReadVector(Params, TEXT("anchor"), Anchor))
					{
						SetFailure(OutFailure, TEXT("voxel_grid_anchor_invalid"));
						return false;
					}
					Session.CellSize = CellSize;
					Session.GridToWorld = FTransform(
						Rotation, Origin, FVector::OneVector);
					Session.AnchorLocal = Anchor;
					FActorSpawnParameters Spawn;
					Spawn.ObjectFlags = RF_Transactional;
					Spawn.Name = MakeUniqueObjectName(
						World.PersistentLevel,
						ADynamicMeshActor::StaticClass(),
						FName(*FString(TEXT("UAI_VoxelGrid_")) + Session.GridId));
					const FTransform InitialTransform = ActorTransform(Session);
					Actor = World.SpawnActor<ADynamicMeshActor>(
						ADynamicMeshActor::StaticClass(),
						InitialTransform.GetLocation(),
						InitialTransform.Rotator(),
						Spawn);
					if (!Actor)
					{
						SetFailure(OutFailure, TEXT("voxel_grid_actor_spawn_failed"));
						return false;
					}
					Session.Actor = Actor;
					Session.bCreatedActor = true;
					Session.bDirty = true;
					Session.bCommitted = false;
					FString Label;
					if (Params->TryGetStringField(TEXT("label"), Label)
						&& !Label.IsEmpty()) Actor->SetActorLabel(Label);
					FString Folder;
					if (Params->TryGetStringField(TEXT("folder_path"), Folder)
						&& !Folder.IsEmpty()) Actor->SetFolderPath(FName(*Folder));
					Actor->Tags.AddUnique(FName(*ActorTag(Session.GridId)));
					if (UDynamicMeshComponent* Component = DynamicMeshComponent(Actor))
					{
						Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
					}
				}
				else
				{
					Actor = ResolveActor(World, ActorRef, Session.GridId, Error);
					if (!Actor)
					{
						SetFailure(OutFailure, Error);
						return false;
					}
					if (Session.GridId.IsEmpty()
						&& !TryGridIdFromActor(*Actor, Session.GridId))
					{
						SetFailure(OutFailure, TEXT("voxel_grid_grid_id_required"));
						return false;
					}
					if (!IsSafeIdentity(Session.GridId))
					{
						SetFailure(OutFailure, TEXT("voxel_grid_grid_id_invalid"));
						return false;
					}
					FString TaggedGridId;
					if (!TryGridIdFromActor(*Actor, TaggedGridId)
						|| TaggedGridId != Session.GridId)
					{
						SetFailure(
							OutFailure, TEXT("voxel_grid_actor_grid_identity_mismatch"));
						return false;
					}
					Session.Actor = Actor;
					Session.SidecarPath = SidecarPath(Session.GridId);
					if (!LoadSidecar(Session, Error))
					{
						SetFailure(OutFailure, Error);
						return false;
					}
				}
				Session.SidecarPath = SidecarPath(Session.GridId);
				Sessions.Add(SessionId, MoveTemp(Session));
				FSession& Stored = Sessions.FindChecked(SessionId);
				OutPayload = SuccessPayload(
					Request, TEXT("begin"), SessionObservation(Stored, Params));
				return true;
			}

			bool MutateBounds(
				const FUeAgentCommandRequest& Request,
				const FString& Action,
				FSession& Session,
				TSharedPtr<FJsonObject>& OutPayload,
				FFailure& OutFailure)
			{
				FString Error;
				if (!CheckRevision(Request.GetParams(), Session, Error))
				{
					SetFailure(OutFailure, Error);
					return false;
				}
				FVoxelGridCellRange Range;
				TSharedPtr<FJsonObject> BoundsReadback;
				if (!ParseBounds(
					Request.GetParams(), Session, Range, BoundsReadback, Error))
				{
					SetFailure(OutFailure, Error);
					return false;
				}
				if (Action == TEXT("add_bounds"))
				{
					int64 NewCells = 0;
					for (int32 Z = Range.Min.Z; Z < Range.MaxExclusive.Z; ++Z)
					{
						for (int32 Y = Range.Min.Y; Y < Range.MaxExclusive.Y; ++Y)
						{
							for (int32 X = Range.Min.X; X < Range.MaxExclusive.X; ++X)
							{
								NewCells += Session.Cells.Contains(FIntVector(X, Y, Z)) ? 0 : 1;
							}
						}
					}
					if (Session.Cells.Num() + NewCells > MaxSessionCells)
					{
						SetFailure(OutFailure, TEXT("voxel_grid_session_cell_limit_exceeded"));
						return false;
					}
				}

				int32 Changed = 0;
				for (int32 Z = Range.Min.Z; Z < Range.MaxExclusive.Z; ++Z)
				{
					for (int32 Y = Range.Min.Y; Y < Range.MaxExclusive.Y; ++Y)
					{
						for (int32 X = Range.Min.X; X < Range.MaxExclusive.X; ++X)
						{
							const FIntVector Cell(X, Y, Z);
							if (Action == TEXT("add_bounds"))
							{
								if (!Session.Cells.Contains(Cell))
								{
									Session.Cells.Add(Cell);
									++Changed;
								}
							}
							else
							{
								Changed += Session.Cells.Remove(Cell);
							}
						}
					}
				}
				++Session.Revision;
				Session.bDirty = true;
				const TSharedRef<FJsonObject> Readback =
					SessionObservation(Session, Request.GetParams());
				Readback->SetNumberField(TEXT("changed_cell_count"), Changed);
				Readback->SetObjectField(TEXT("bounds"), BoundsReadback);
				OutPayload = SuccessPayload(Request, Action, Readback);
				return true;
			}

			bool Close(
				const FUeAgentCommandRequest& Request,
				const FString& Action,
				FSession& Session,
				TSharedPtr<FJsonObject>& OutPayload,
				FFailure& OutFailure)
			{
				FString Error;
				if (!CheckRevision(Request.GetParams(), Session, Error))
				{
					SetFailure(OutFailure, Error);
					return false;
				}
				if (Action == TEXT("close") && Session.bDirty)
				{
					SetFailure(OutFailure, TEXT("voxel_grid_session_dirty"),
						TEXT("apply or cancel the session before close"));
					return false;
				}
				const FString SessionId = Session.SessionId;
				const bool bDestroyActor =
					Action == TEXT("cancel")
					&& Session.bCreatedActor && !Session.bCommitted;
				bool bActorReleased = true;
				if (bDestroyActor)
				{
					ADynamicMeshActor* const Actor = Session.Actor.Get();
					bActorReleased = Actor && Actor->GetWorld()
						? Actor->GetWorld()->DestroyActor(Actor) : false;
				}
				const TSharedRef<FJsonObject> Readback = MakeShared<FJsonObject>();
				Readback->SetStringField(TEXT("session_id"), SessionId);
				Readback->SetBoolField(TEXT("released"), true);
				Readback->SetBoolField(TEXT("actor_destroyed"), bDestroyActor);
				Readback->SetBoolField(TEXT("actor_release_confirmed"), bActorReleased);
				Sessions.Remove(SessionId);
				if (!bActorReleased)
				{
					SetFailure(
						OutFailure,
						TEXT("voxel_grid_cancel_cleanup_unconfirmed"),
						SessionId,
						TEXT("state_unknown"),
						false,
						true);
					return false;
				}
				OutPayload = SuccessPayload(Request, Action, Readback);
				return true;
			}

			FCriticalSection Mutex;
			TMap<FString, FSession> Sessions;
		};

		FProcessRegistry& ProcessRegistry()
		{
			static FProcessRegistry Registry;
			return Registry;
		}

		class FMechanicsHandler final : public IUeAgentCommandHandler
		{
		public:
			explicit FMechanicsHandler(FString InCommandId)
				: CommandId(MoveTemp(InCommandId))
			{
			}

			const FString& GetCommandId() const override { return CommandId; }

			FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const Contract::FUeAgentCallerContext& Caller,
				const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation)
				override
			{
				FFailure Failure;
				if (Request.GetCommandId() != CommandId)
				{
					SetFailure(Failure, TEXT("voxel_grid_command_identity_mismatch"));
					return FUeAgentCommandResult::Failure(
						Request, TEXT("{}"), ErrorJson(CommandId, Failure));
				}
				if (!Caller.IsValid()
					|| !Caller.GetAuthorityScopes().Contains(
						TEXT("uai:modeling-geometry:write")))
				{
					SetFailure(Failure, TEXT("modeling_geometry_authority_required"));
					return FUeAgentCommandResult::Failure(
						Request, TEXT("{}"), ErrorJson(CommandId, Failure));
				}
				if (Cancellation->IsCancelled())
				{
					SetFailure(Failure, TEXT("operation_cancelled"));
					return FUeAgentCommandResult::Failure(
						Request, TEXT("{}"), ErrorJson(CommandId, Failure));
				}
				TSharedPtr<FJsonObject> Payload;
				const bool bOk = CommandId == GetSessionCommandId()
					? ProcessRegistry().ExecuteSession(Request, Payload, Failure)
					: ProcessRegistry().ExecuteApply(Request, Payload, Failure);
				return bOk && Payload.IsValid()
					? FUeAgentCommandResult::Success(Request, JsonText(Payload.ToSharedRef()))
					: FUeAgentCommandResult::Failure(
						Request, TEXT("{}"), ErrorJson(CommandId, Failure));
			}

		private:
			FString CommandId;
		};
	}

	bool FVoxelGridCellRange::IsValid() const
	{
		return MaxExclusive.X > Min.X
			&& MaxExclusive.Y > Min.Y
			&& MaxExclusive.Z > Min.Z;
	}

	int64 FVoxelGridCellRange::NumCells() const
	{
		if (!IsValid()) return 0;
		return (static_cast<int64>(MaxExclusive.X) - Min.X)
			* (static_cast<int64>(MaxExclusive.Y) - Min.Y)
			* (static_cast<int64>(MaxExclusive.Z) - Min.Z);
	}

	bool FVoxelGridCollisionBox::IsValid() const
	{
		return MaxExclusive.X > Min.X
			&& MaxExclusive.Y > Min.Y
			&& MaxExclusive.Z > Min.Z;
	}

	int64 FVoxelGridCollisionBox::NumCells() const
	{
		if (!IsValid()) return 0;
		return (static_cast<int64>(MaxExclusive.X) - Min.X)
			* (static_cast<int64>(MaxExclusive.Y) - Min.Y)
			* (static_cast<int64>(MaxExclusive.Z) - Min.Z);
	}

	const FString& GetSessionCommandId()
	{
		static const FString Id(TEXT("mesh.voxel_grid.session"));
		return Id;
	}

	const FString& GetApplyCommandId()
	{
		static const FString Id(TEXT("mesh.voxel_grid.apply"));
		return Id;
	}

	const TArray<FString>& GetCommandIds()
	{
		static const TArray<FString> Ids = {
			GetSessionCommandId(), GetApplyCommandId(),
		};
		return Ids;
	}

	bool IsCommand(const FString& CommandId)
	{
		return GetCommandIds().Contains(CommandId);
	}

	bool TryConvertBoundsToCellRange(
		const FBox& Bounds,
		const FString& InputSpace,
		const FTransform& GridToWorld,
		const FVector& AnchorLocal,
		const double CellSize,
		FVoxelGridCellRange& OutRange,
		FString& OutError)
	{
		OutRange = {};
		OutError.Reset();
		if (!Bounds.IsValid
			|| !IsFiniteVector(Bounds.Min)
			|| !IsFiniteVector(Bounds.Max)
			|| !FMath::IsFinite(CellSize)
			|| CellSize <= KINDA_SMALL_NUMBER)
		{
			OutError = TEXT("voxel_grid_bounds_invalid");
			return false;
		}
		FBox GridBounds(ForceInit);
		if (InputSpace == TEXT("world"))
		{
			for (int32 X = 0; X < 2; ++X)
			{
				for (int32 Y = 0; Y < 2; ++Y)
				{
					for (int32 Z = 0; Z < 2; ++Z)
					{
						GridBounds += GridToWorld.InverseTransformPosition(FVector(
							X ? Bounds.Max.X : Bounds.Min.X,
							Y ? Bounds.Max.Y : Bounds.Min.Y,
							Z ? Bounds.Max.Z : Bounds.Min.Z));
					}
				}
			}
		}
		else if (InputSpace == TEXT("grid_local"))
		{
			GridBounds = Bounds;
		}
		else if (InputSpace == TEXT("component_local"))
		{
			GridBounds = FBox(Bounds.Min + AnchorLocal, Bounds.Max + AnchorLocal);
		}
		else
		{
			OutError = TEXT("voxel_grid_bounds_space_unsupported");
			return false;
		}
		const FVector ScaledMin = GridBounds.Min / CellSize;
		const FVector ScaledMax = GridBounds.Max / CellSize;
		const double Minimum = static_cast<double>(MIN_int32) + 1.0;
		const double Maximum = static_cast<double>(MAX_int32) - 1.0;
		if (ScaledMin.GetMin() < Minimum || ScaledMin.GetMax() > Maximum
			|| ScaledMax.GetMin() < Minimum || ScaledMax.GetMax() > Maximum)
		{
			OutError = TEXT("voxel_grid_bounds_coordinate_overflow");
			return false;
		}
		OutRange.Min = FIntVector(
			FMath::FloorToInt(ScaledMin.X),
			FMath::FloorToInt(ScaledMin.Y),
			FMath::FloorToInt(ScaledMin.Z));
		OutRange.MaxExclusive = FIntVector(
			FMath::CeilToInt(ScaledMax.X),
			FMath::CeilToInt(ScaledMax.Y),
			FMath::CeilToInt(ScaledMax.Z));
		if (!OutRange.IsValid())
		{
			OutError = TEXT("voxel_grid_bounds_empty");
			return false;
		}
		return true;
	}

	TArray<FVoxelGridCollisionBox> BuildExactCoverBoxes(
		const TSet<FIntVector>& Cells)
	{
		TSet<FIntVector> Remaining = Cells;
		TArray<FVoxelGridCollisionBox> Boxes;
		while (!Remaining.IsEmpty())
		{
			const FIntVector Start = SortedCells(Remaining)[0];
			FIntVector Min = Start;
			FIntVector MaxExclusive = Start + FIntVector(1, 1, 1);
			while (AreCellsPresent(
				Remaining,
				FIntVector(MaxExclusive.X, Min.Y, Min.Z),
				FIntVector(MaxExclusive.X + 1, MaxExclusive.Y, MaxExclusive.Z)))
			{
				++MaxExclusive.X;
			}
			while (AreCellsPresent(
				Remaining,
				FIntVector(Min.X, MaxExclusive.Y, Min.Z),
				FIntVector(MaxExclusive.X, MaxExclusive.Y + 1, MaxExclusive.Z)))
			{
				++MaxExclusive.Y;
			}
			while (AreCellsPresent(
				Remaining,
				FIntVector(Min.X, Min.Y, MaxExclusive.Z),
				FIntVector(MaxExclusive.X, MaxExclusive.Y, MaxExclusive.Z + 1)))
			{
				++MaxExclusive.Z;
			}
			Boxes.Add({Min, MaxExclusive});
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

	bool TryCreateMechanicsHandler(
		const FString& CommandId,
		TSharedPtr<IUeAgentCommandHandler>& OutHandler,
		FString& OutError)
	{
		OutHandler.Reset();
		OutError.Reset();
		if (!IsCommand(CommandId))
		{
			OutError = TEXT("voxel_grid_mechanics_command_unknown:") + CommandId;
			return false;
		}
		OutHandler = MakeShared<FMechanicsHandler>(CommandId);
		return true;
	}

#if WITH_DEV_AUTOMATION_TESTS
	void ResetProcessSessionsForAutomation()
	{
		ProcessRegistry().Reset();
	}
#endif
}

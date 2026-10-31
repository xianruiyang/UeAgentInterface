#include "DomainAdapters/ModelingGeometry/UeAgentModelingVoxelGrid.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

#include "DomainAdapters/ModelingGeometry/UeAgentModelingVoxelGridState.h"
#include "DomainAdapters/ModelingGeometry/UeAgentModelingMechanics.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "Components/BoxComponent.h"
#include "Components/DynamicMeshComponent.h"
#include "Core/AssetDurable/UeAgentAssetDurable.h"
#include "Core/AssetDurable/UeAgentPersistedWorld.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMesh/DynamicMeshOverlay.h"
#include "DynamicMeshActor.h"
#include "Editor.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "Materials/MaterialInterface.h"
#include "DomainAdapters/ModelingGeometry/UeAgentModelingMetricMaterial.h"
#include "PhysicsEngine/AggregateGeom.h"
#include "PhysicsEngine/BodySetup.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Misc/ScopeLock.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UDynamicMesh.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringState.h"

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END

namespace UE::UAI::DomainAdapters::ModelingGeometry::VoxelGrid
{
	using namespace CommandEdge;
	using namespace AssetDurable;
	using namespace ContextResource;
	using namespace PlanningPreflight;
	using namespace StateSemantic;
	using UE::Geometry::FDynamicMesh3;
	using UE::Geometry::FDynamicMeshMaterialAttribute;
	using UE::Geometry::FDynamicMeshNormalOverlay;
	using UE::Geometry::FDynamicMeshUVOverlay;
	using UE::Geometry::FIndex3i;
	using UeAgentModelingOps::ModelingFact;
	using UeAgentModelingOps::ModelingCondition;
	using UeAgentModelingOps::ModelingInputFailure;

	namespace
	{
		constexpr const TCHAR* ActorTagPrefix = TEXT("UAIEditableVoxelGrid:");
		constexpr const TCHAR* CollisionTagPrefix =
			TEXT("UAIEditableVoxelGridCollision:");
		constexpr const TCHAR* CollisionGenerationTagPrefix =
			TEXT("UAIEditableVoxelGridCollisionGeneration:");
		constexpr const TCHAR* LegacySidecarSchemaV2 =
			TEXT("uai.modeling.voxel_grid.v2");
		constexpr const TCHAR* LegacySidecarSchema =
			TEXT("uai_editable_voxel_grid_v1");

#if WITH_DEV_AUTOMATION_TESTS
		TWeakObjectPtr<UWorld> GAutomationWorldOverride;
#endif

		using FFailure = UE::UAI::Contract::FDiagnostic;

		TSharedPtr<FJsonValue> GridNumberFact(double Value)
		{
			return FMath::IsFinite(Value) ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(Value))
				: TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(FString::Printf(TEXT("%.17g"), Value)));
		}

		TSharedRef<FJsonObject> GridVectorFact(const FVector& Value)
		{
			const auto Result = MakeShared<FJsonObject>();
			Result->SetField(TEXT("x"), GridNumberFact(Value.X));
			Result->SetField(TEXT("y"), GridNumberFact(Value.Y));
			Result->SetField(TEXT("z"), GridNumberFact(Value.Z));
			return Result;
		}

		struct FSession final
		{
			FString SessionId;
			FString GridId;
			TWeakObjectPtr<ADynamicMeshActor> Actor;
			double CellSize = 100.0;
			FTransform GridToWorld = FTransform::Identity;
			FVector AnchorLocal = FVector::ZeroVector;
			TSet<FIntVector> Cells;
			bool bSolidGeometry = false;
			TArray<FUeAgentGridSolidPiece> SolidPieces;
			int64 Revision = 0;
			bool bDirty = false;
			bool bCreatedActor = false;
			bool bCommitted = false;
			bool bStateUnknown = false;
			FString LegacySidecarPath;
			bool bLegacySidecarImported = false;
			FString CollisionGeneration;
			FString CollisionMode = TEXT("none");
			FAssetDurableSessionPtr DurableSession;
			TSharedPtr<FUeAgentResourceTracker> DurableResources;
			int64 FencingToken = -1;
			FString DurablePlanFingerprint;
			FString DurableJournalOwnerId;
			FString DurableReceiptId;
		};

		FFailure SessionInputFailure(const TSharedPtr<FJsonObject>& Params)
		{
			FFailure Error = ModelingCondition(TEXT("voxel_grid_session_id_required"),
				TEXT("A managed voxel session reference must be supplied as a string."),
				TEXT("/session_id"), false, true);
			Error.SourceKind = TEXT("command_input");
			Error.Source.JsonPath = TEXT("/session_id");
			Error.SemanticPointer = Error.Source.JsonPath;
			Error.PublicMessage += Params.IsValid() && Params->HasField(TEXT("session_id"))
				? TEXT(" The supplied field has the wrong type.") : TEXT(" The field is missing.");
			return Error;
		}

		FFailure SessionLookupFailure(const FString& SessionId, const FSession* Session)
		{
			const TSharedRef<FJsonObject> Actual = MakeShared<FJsonObject>();
			Actual->SetBoolField(TEXT("session_found"), Session != nullptr);
			Actual->SetBoolField(TEXT("actor_available"), Session && Session->Actor.IsValid());
			const TSharedRef<FJsonObject> Expected = MakeShared<FJsonObject>();
			Expected->SetBoolField(TEXT("session_found"), true);
			Expected->SetBoolField(TEXT("actor_available"), true);
			FFailure Error = ModelingFact(TEXT("voxel_grid_session_not_found"), Session
				? TEXT("The voxel session exists, but its actor is no longer available.")
				: TEXT("No active voxel session matches the managed session reference."),
				TEXT("/session"), MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueObject>(Expected));
			Error.ActualEvidence = MakeShared<FJsonValueString>(SessionId);
			return Error;
		}

		FString MakeGuid(const TCHAR* Prefix)
		{
			return FString(Prefix)
				+ FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(16);
		}

		FString Sha256(const FString& Value)
		{
			const FTCHARToUTF8 Utf8(*Value);
			FSHA256Signature Signature{};
			const uint8 Empty = 0;
			const uint8* Data = Utf8.Length() > 0
				? reinterpret_cast<const uint8*>(Utf8.Get()) : &Empty;
			if (!SHA256(
					Data,
					static_cast<size_t>(Utf8.Length()),
					Signature.Signature))
			{
				return FString();
			}
			return TEXT("sha256:") + Signature.ToString().ToLower();
		}

		TSharedRef<FUeAgentResourceTracker> MakeDurableResources()
		{
			return MakeShared<FUeAgentResourceTracker>();
		}

		FUeAgentCanonicalIdentity DurableIdentity(const FSession& Session)
		{
			return FUeAgentCanonicalIdentity(
				TEXT("uai.semantic.modeling.voxel_grid"),
				TEXT("ue.dynamic_mesh_actor"),
				Session.GridId,
				{{TEXT("grid_id"), Session.GridId}},
				TEXT("world_actor"),
				TEXT("modeling-voxel-grid"));
		}

		FPlanningPreflightPlanRef MakeDurablePlan(
			const FUeAgentCommandRequest& Request,
			const FSession& Session,
			const bool bCreate)
		{
			const FUeAgentCanonicalIdentity Identity = DurableIdentity(Session);
			TArray<FPlannedChange> Changes;
			Changes.Emplace(
				Identity,
				bCreate ? EChangeIntent::Create : EChangeIntent::Update,
				FUeAgentCanonicalValuePtr(),
				FUeAgentCanonicalValuePtr(),
				TArray<FString>{TEXT("uai:operate")},
				TEXT("modeling-voxel-grid-durable"));
			const FString Fingerprint = Sha256(
				Request.GetCommandId() + TEXT("\n")
				+ Request.GetCommandBindingHash() + TEXT("\n")
				+ Request.GetRequestId() + TEXT("\n")
				+ Session.SessionId + TEXT("\n")
				+ Session.GridId);
			return MakeShared<const FPlanningPreflightPlan>(
				Contract::FUeAgentDescriptorIdentity(
					Request.GetCommandId(), 1, Request.GetCommandBindingHash()),
				Request.GetRequestId(),
				Session.SessionId,
				TEXT("modeling-voxel-grid"),
				TArray<FString>{TEXT("uai:operate")},
				TEXT("voxel-grid:") + Session.GridId,
				FUeAgentStateRevision(
					TEXT("modeling_voxel_grid"),
					Session.GridId,
					Session.Revision),
				TEXT("modeling-voxel-grid-session"),
				FChangeSet(MoveTemp(Changes)),
				FImpactSet(),
				TArray<FPrecondition>{},
				Fingerprint);
		}

		void SetFailure(FFailure& OutFailure, UE::UAI::Contract::FDiagnostic Diagnostic,
            const FString& TerminalStatus = TEXT("not_started"), const bool bRetrySafe = true,
            const bool bMutationStarted = false, const UE::UAI::Contract::FDiagnostic& Recovery = UE::UAI::Contract::FDiagnostic())
        {
            OutFailure = MoveTemp(Diagnostic);
            OutFailure.TerminalStatus = TerminalStatus;
            OutFailure.bRetrySafe = bRetrySafe;
            OutFailure.MutationStarted = bMutationStarted;
            OutFailure.Append(Recovery);
        }

		UE::UAI::Contract::FDiagnostic ErrorDiagnostic(const FString& SubjectId, const FFailure& Failure)
        { auto Diagnostic = Failure; if (Diagnostic.SubjectId.IsEmpty()) Diagnostic.SubjectId = SubjectId; return Diagnostic; }

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
			FVector& OutValue, FFailure* OutError = nullptr)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (!Parent.IsValid()
				|| !Parent->TryGetObjectField(Field, Object)
				|| !Object || !Object->IsValid())
			{
				if (OutError) *OutError = ModelingInputFailure(TEXT("voxel_grid_vector_invalid"), TEXT("This voxel vector field requires an object with numeric x, y and z coordinates."), Parent, Field, MakeShared<FJsonValueString>(TEXT("object with finite x, y, z")));
				return false;
			}
			const TCHAR* Axes[] = {TEXT("x"), TEXT("y"), TEXT("z")};
			double Values[] = {0.0, 0.0, 0.0};
			for (int32 Index = 0; Index < 3; ++Index)
				if (!(*Object)->TryGetNumberField(Axes[Index], Values[Index]))
				{
					if (OutError) {
						*OutError = ModelingInputFailure(TEXT("voxel_grid_vector_coordinate_invalid"), TEXT("The voxel vector coordinate must be a number."), *Object, Axes[Index], MakeShared<FJsonValueString>(TEXT("number")));
						UeAgentAuthoringDiagnostics::PrefixDocumentFailure(*OutError, TEXT("/") + FString(Field), false, true);
					}
					return false;
				}
			OutValue = FVector(Values[0], Values[1], Values[2]);
			for (int32 Index = 0; Index < 3; ++Index)
				if (!FMath::IsFinite(OutValue[Index]))
				{
					if (OutError) {
						*OutError = ModelingInputFailure(TEXT("voxel_grid_vector_coordinate_invalid"), TEXT("The voxel vector coordinate must be finite."), *Object, Axes[Index], MakeShared<FJsonValueString>(TEXT("finite number")));
						UeAgentAuthoringDiagnostics::PrefixDocumentFailure(*OutError, TEXT("/") + FString(Field), false, true);
					}
					return false;
				}
			return true;
		}

		bool TryReadRotator(
			const TSharedPtr<FJsonObject>& Parent,
			const TCHAR* Field,
			FRotator& OutValue, FFailure* OutError = nullptr)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (!Parent.IsValid()
				|| !Parent->TryGetObjectField(Field, Object)
				|| !Object || !Object->IsValid())
			{
				if (OutError) *OutError = ModelingInputFailure(TEXT("voxel_grid_rotation_invalid"), TEXT("This voxel rotation requires an object with numeric pitch, yaw and roll."), Parent, Field, MakeShared<FJsonValueString>(TEXT("object with finite pitch, yaw, roll")));
				return false;
			}
			const TCHAR* Axes[] = {TEXT("pitch"), TEXT("yaw"), TEXT("roll")};
			double Values[] = {0.0, 0.0, 0.0};
			for (int32 Index = 0; Index < 3; ++Index)
				if (!(*Object)->TryGetNumberField(Axes[Index], Values[Index]))
				{
					if (OutError) {
						*OutError = ModelingInputFailure(TEXT("voxel_grid_rotation_coordinate_invalid"), TEXT("The voxel rotation coordinate must be a number."), *Object, Axes[Index], MakeShared<FJsonValueString>(TEXT("number")));
						UeAgentAuthoringDiagnostics::PrefixDocumentFailure(*OutError, TEXT("/") + FString(Field), false, true);
					}
					return false;
				}
			for (int32 Index = 0; Index < 3; ++Index)
				if (!FMath::IsFinite(Values[Index]))
				{
					if (OutError) {
						*OutError = ModelingInputFailure(TEXT("voxel_grid_rotation_coordinate_invalid"), TEXT("The voxel rotation coordinate must be finite."), *Object, Axes[Index], MakeShared<FJsonValueString>(TEXT("finite number")));
						UeAgentAuthoringDiagnostics::PrefixDocumentFailure(*OutError, TEXT("/") + FString(Field), false, true);
					}
					return false;
				}
			OutValue = FRotator(Values[0], Values[1], Values[2]);
			return true;
		}

		bool TryReadIntegralField(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field,
			int64& OutValue, FFailure* OutError = nullptr)
		{
			double Number = 0.0;
			if (!Object.IsValid()
				|| !Object->TryGetNumberField(Field, Number)
				|| !FMath::IsFinite(Number)
				|| !FMath::IsNearlyEqual(Number, FMath::RoundToDouble(Number), 1.e-6)
				|| Number < static_cast<double>(MIN_int64)
				|| Number >= -static_cast<double>(MIN_int64))
			{
				if (OutError) *OutError = ModelingInputFailure(TEXT("voxel_grid_integer_invalid"), TEXT("This voxel field requires a finite integer representable as a signed 64-bit value."), Object, Field, MakeShared<FJsonValueString>(TEXT("finite signed 64-bit integer")));
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

		FString LegacySidecarPath(const FString& GridId)
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

		bool TryReadCellValue(
			const TSharedPtr<FJsonValue>& Value,
			FIntVector& OutCell, FFailure* OutError = nullptr)
		{
			const TSharedPtr<FJsonObject> Object =
				Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
			if (!Object.IsValid())
			{
				if (OutError) {
					TSharedPtr<FJsonValue> Actual = Value;
					if (Value.IsValid() && Value->Type == EJson::Array) {
						const auto Summary = MakeShared<FJsonObject>();
						Summary->SetStringField(TEXT("kind"), TEXT("array")); Summary->SetNumberField(TEXT("count"), Value->AsArray().Num());
						Actual = MakeShared<FJsonValueObject>(Summary);
					}
					*OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("voxel_grid_cell_invalid"), TEXT("/"), MakeShared<FJsonValueString>(TEXT("object with integer x, y, z")),
						Actual, TEXT("voxel_grid_input"), TEXT("Each voxel cell must be an object with integer x, y and z coordinates."));
					OutError->SourceKind = TEXT("command_input");
				}
				return false;
			}
			const TCHAR* Axes[] = {TEXT("x"), TEXT("y"), TEXT("z")};
			int64 Values[] = {0, 0, 0};
			for (int32 Index = 0; Index < 3; ++Index)
				if (!TryReadIntegralField(Object, Axes[Index], Values[Index], OutError)) return false;
			for (int32 Index = 0; Index < 3; ++Index)
				if (Values[Index] <= MIN_int32 || Values[Index] >= MAX_int32)
				{
					if (OutError) *OutError = ModelingInputFailure(TEXT("voxel_grid_cell_coordinate_out_of_range"), TEXT("The voxel cell coordinate must lie strictly inside the signed 32-bit limits."), Object, Axes[Index], MakeShared<FJsonValueString>(TEXT("-2147483648 < coordinate < 2147483647")));
					return false;
				}
			OutCell = FIntVector(
				static_cast<int32>(Values[0]),
				static_cast<int32>(Values[1]),
				static_cast<int32>(Values[2]));
			return true;
		}

		void LocateLegacySidecarFailure(FFailure& Error, const FSession& Session)
		{
			for (auto& Child : Error.Diagnostics) LocateLegacySidecarFailure(Child, Session);
			if (Error.PublicCause) {
				auto Cause = MakeShared<FFailure>(*Error.PublicCause);
				LocateLegacySidecarFailure(*Cause, Session); Error.PublicCause = MoveTemp(Cause);
			}
			if (!Error.Source.SourceFile.IsEmpty() && Error.Source.SourceFile != Session.LegacySidecarPath) return;
			const FString Pointer = !Error.SemanticPointer.IsEmpty() ? Error.SemanticPointer
				: !Error.Source.InternalJsonPath.IsEmpty() ? Error.Source.InternalJsonPath
				: !Error.Source.JsonPath.IsEmpty() ? Error.Source.JsonPath : Error.PropertyPath;
			Error.Source.SourceFile = Session.LegacySidecarPath;
			Error.Source.JsonPath = Pointer.StartsWith(TEXT("/")) ? Pointer : TEXT("/");
			Error.SourceKind = TEXT("native_result");
			if (Error.TargetPath.IsEmpty() && Session.Actor.IsValid()) Error.TargetPath = Session.Actor->GetPathName();
		}

		bool LoadLegacySidecar(
			FSession& Session,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutError.Reset();
			ON_SCOPE_EXIT { if (!OutError.IsEmpty()) LocateLegacySidecarFailure(OutError, Session); };
			FString Text;
			if (!FFileHelper::LoadFileToString(Text, *Session.LegacySidecarPath))
			{
				OutError = ModelingCondition(TEXT("voxel_grid_sidecar_missing"), Session.LegacySidecarPath.IsEmpty() ? TEXT("The managed voxel legacy sidecar has no file path.") : TEXT("The managed voxel legacy sidecar file could not be read."), TEXT("/sidecar_read"), false, true);
				return false;
			}
			TSharedPtr<FJsonObject> Root;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
			if (!FJsonSerializer::Deserialize(
				Reader, Root)
				|| !Root.IsValid())
			{
				OutError = ModelingCondition(TEXT("voxel_grid_sidecar_parse_failed"), Reader->GetErrorMessage().IsEmpty() ? TEXT("The voxel legacy sidecar must contain a JSON object at its root.") : Reader->GetErrorMessage(), TEXT("/parsed_root_object_present"), false, true);
				OutError.Stage = TEXT("voxel_legacy_parse");
				OutError.Source.JsonPath = TEXT("/");
				OutError.Source.Line = Reader->GetLineNumber(); OutError.Source.Column = Reader->GetCharacterNumber();
				return false;
			}
			FString Schema;
			FString GridId;
			double CellSize = 0.0;
			int64 Revision = 0;
			FVector Origin = FVector::ZeroVector;
			FRotator Rotation = FRotator::ZeroRotator;
			FVector Anchor = FVector::ZeroVector;
			const bool bSchemaString = Root->TryGetStringField(TEXT("schema"), Schema);
			if (!bSchemaString
				|| (Schema != LegacySidecarSchemaV2
					&& Schema != LegacySidecarSchema))
			{
				OutError = ModelingCondition(TEXT("voxel_grid_sidecar_contract_invalid"), bSchemaString ? TEXT("The voxel legacy sidecar uses an unsupported storage schema.") : Root->HasField(TEXT("schema")) ? TEXT("The legacy sidecar schema marker must be a string.") : TEXT("The legacy sidecar has no schema marker."), TEXT("/storage_schema_supported"), false, true);
				OutError.Source.JsonPath = TEXT("/schema");
				OutError.ActualEvidence = Root->Values.FindRef(TEXT("schema"));
				OutError.ExpectedEvidence = MakeShared<FJsonValueString>(FString(LegacySidecarSchemaV2) + TEXT(" or ") + LegacySidecarSchema);
				return false;
			}
			const bool bGridIdString = Root->TryGetStringField(TEXT("grid_id"), GridId);
			if (!bGridIdString || GridId != Session.GridId)
			{
				OutError = ModelingCondition(TEXT("voxel_grid_sidecar_contract_invalid"), bGridIdString ? TEXT("The legacy sidecar grid identity differs from the managed grid identity.") : Root->HasField(TEXT("grid_id")) ? TEXT("The legacy sidecar grid identity must be a string.") : TEXT("The legacy sidecar has no managed grid identity."), TEXT("/grid_identity_matches"), false, true);
				OutError.Source.JsonPath = TEXT("/grid_id");
				OutError.ActualEvidence = Root->Values.FindRef(TEXT("grid_id"));
				OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Session.GridId);
				return false;
			}
			if (!Root->TryGetNumberField(TEXT("cell_size"), CellSize) || !FMath::IsFinite(CellSize) || CellSize <= KINDA_SMALL_NUMBER)
			{
				OutError = ModelingInputFailure(TEXT("voxel_grid_sidecar_contract_invalid"), TEXT("The legacy sidecar cell_size must be finite and greater than the engine's small-number tolerance."), Root, TEXT("cell_size"), MakeShared<FJsonValueString>(FString::Printf(TEXT("finite number > %.17g"), static_cast<double>(KINDA_SMALL_NUMBER))));
				return false;
			}
			if (!TryReadVector(Root, TEXT("grid_origin"), Origin, &OutError) || !TryReadRotator(Root, TEXT("grid_rotation"), Rotation, &OutError))
			{
				OutError = FFailure::WrapFailure(TEXT("voxel_grid_sidecar_contract_invalid"), OutError, TEXT("voxel_legacy_read"), TEXT("The legacy sidecar grid origin or rotation is invalid."));
				return false;
			}
			if (!TryReadIntegralField(Root, TEXT("revision"), Revision) || Revision < 0)
			{
				OutError = ModelingCondition(TEXT("voxel_grid_sidecar_contract_invalid"), TEXT("The legacy sidecar managed revision must be a non-negative signed 64-bit integer."), TEXT("/managed_revision_valid"), false, true);
				OutError.Source.JsonPath = TEXT("/revision");
				OutError.ActualEvidence = Root->Values.FindRef(TEXT("revision"));
				OutError.ExpectedEvidence = MakeShared<FJsonValueString>(TEXT("non-negative signed 64-bit integer"));
				return false;
			}
			TryReadVector(Root, TEXT("anchor"), Anchor);
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Root->TryGetArrayField(TEXT("cells"), Values)
				|| !Values)
			{
				OutError = ModelingInputFailure(TEXT("voxel_grid_sidecar_cells_invalid"), TEXT("The legacy sidecar requires a cells array."), Root, TEXT("cells"), MakeShared<FJsonValueString>(TEXT("array")));
				return false;
			}
			TSet<FIntVector> Cells;
			int32 FirstDuplicateIndex = INDEX_NONE;
			FIntVector FirstDuplicateCell = FIntVector::ZeroValue;
			for (int32 Index = 0; Index < Values->Num(); ++Index)
			{
				const TSharedPtr<FJsonValue>& Value = (*Values)[Index];
				FIntVector Cell;
				if (!TryReadCellValue(Value, Cell, &OutError))
				{
					UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, FString::Printf(TEXT("/cells/%d"), Index), false, true);
					OutError = FFailure::WrapFailure(TEXT("voxel_grid_sidecar_cell_invalid"), OutError, TEXT("voxel_legacy_read"), TEXT("The legacy sidecar contains an invalid cell coordinate."));
					return false;
				}
				if (FirstDuplicateIndex == INDEX_NONE && Cells.Contains(Cell)) { FirstDuplicateIndex = Index; FirstDuplicateCell = Cell; }
				Cells.Add(Cell);
			}
			if (Cells.Num() != Values->Num())
			{
				OutError = ModelingFact(TEXT("voxel_grid_sidecar_cell_duplicate"), TEXT("The legacy sidecar repeats this cell coordinate."), FString::Printf(TEXT("/cells/%d"), FirstDuplicateIndex), MakeShared<FJsonValueString>(FirstDuplicateCell.ToString()), MakeShared<FJsonValueString>(TEXT("unique cell coordinate")));
				return false;
			}
			Session.CellSize = CellSize;
			Session.GridToWorld = FTransform(Rotation, Origin, FVector::OneVector);
			Session.AnchorLocal = Anchor;
			Session.Cells = MoveTemp(Cells);
			Session.Revision = Revision;
			Session.bDirty = false;
			Session.bCommitted = true;
			Session.bLegacySidecarImported = true;
			Root->TryGetStringField(
				TEXT("collision_generation"), Session.CollisionGeneration);
			Root->TryGetStringField(TEXT("collision_mode"), Session.CollisionMode);
			return true;
		}

		UWorld* EditorWorld(FFailure& OutFailure)
		{
			if (!IsInGameThread())
			{
				SetFailure(OutFailure, ModelingCondition(TEXT("voxel_grid_game_thread_required"),
					TEXT("Voxel editor operations require the game thread."), TEXT("/game_thread"), false, true),
					TEXT("not_started"), true, false, UE::UAI::Contract::FDiagnostic());
				return nullptr;
			}
			UWorld* const World =
#if WITH_DEV_AUTOMATION_TESTS
				GAutomationWorldOverride.IsValid()
					? GAutomationWorldOverride.Get()
					:
#endif
					(GEditor ? GEditor->GetEditorWorldContext().World() : nullptr);
			if (!World)
			{
				SetFailure(OutFailure, ModelingCondition(TEXT("voxel_grid_editor_world_unavailable"),
					TEXT("No editor world is available for the voxel operation."), TEXT("/editor_world_available"), false, true),
					TEXT("not_started"), true, false, UE::UAI::Contract::FDiagnostic());
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
			UE::UAI::Contract::FDiagnostic& OutError)
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
				const FString Reason = !ActorRef.IsEmpty()
					? (Matches.IsEmpty()
						? FString::Printf(TEXT("Actor reference '%s' matched no DynamicMeshActor."), *ActorRef)
						: FString::Printf(TEXT("Actor reference '%s' matched multiple DynamicMeshActors; use an exact actor object path."), *ActorRef))
					: GridId.IsEmpty() ? TEXT("No actor reference or managed grid identity is available to resolve the voxel actor.")
					: Matches.IsEmpty() ? TEXT("No DynamicMeshActor carries the managed voxel grid identity.") : TEXT("Multiple DynamicMeshActors carry the same managed voxel grid identity.");
				OutError = ModelingFact(Matches.IsEmpty()
					? FString(TEXT("voxel_grid_actor_not_found"))
					: FString(TEXT("voxel_grid_actor_ambiguous")), Reason, TEXT("/actor_match_count"), MakeShared<FJsonValueNumber>(Matches.Num()), MakeShared<FJsonValueNumber>(1), ActorRef);
				OutError.Stage = TEXT("voxel_actor_resolve");
				if (!ActorRef.IsEmpty()) { OutError.SourceKind = TEXT("related_input"); OutError.Source.JsonPath = TEXT("/actor_ref"); }
				else OutError.ActualEvidence = MakeShared<FJsonValueString>(GridId);
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
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			const TSharedPtr<FJsonObject>* BoundsObject = nullptr;
			if (!Params.IsValid()
				|| !Params->TryGetObjectField(TEXT("bounds"), BoundsObject)
				|| !BoundsObject || !BoundsObject->IsValid())
			{
				OutError = ModelingInputFailure(TEXT("voxel_grid_bounds_required"), TEXT("Voxel editing requires a bounds object."), Params, TEXT("bounds"), MakeShared<FJsonValueString>(TEXT("object")));
				return false;
			}
			FVector Min = FVector::ZeroVector;
			FVector Max = FVector::ZeroVector;
			FFailure MinMaxError;
			if (!TryReadVector(*BoundsObject, TEXT("min"), Min, &MinMaxError)
				|| !TryReadVector(*BoundsObject, TEXT("max"), Max, &MinMaxError))
			{
				FVector Center = FVector::ZeroVector;
				FVector Extent = FVector::ZeroVector;
				FFailure CenterExtentError;
				if (!TryReadVector(*BoundsObject, TEXT("center"), Center, &CenterExtentError)
					|| !TryReadVector(*BoundsObject, TEXT("extent"), Extent, &CenterExtentError)
					|| Extent.GetMin() <= 0.0)
				{
					if (CenterExtentError.IsEmpty()) {
						CenterExtentError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("voxel_grid_bounds_extent_invalid"), TEXT("/extent"), MakeShared<FJsonValueString>(TEXT("positive x, y and z extents")), MakeShared<FJsonValueObject>(GridVectorFact(Extent)), TEXT("voxel_grid_input"), TEXT("Each voxel bounds extent must be positive."));
						CenterExtentError.SourceKind = TEXT("command_input");
					}
					const bool bMinMaxInput = (*BoundsObject)->HasField(TEXT("min")) || (*BoundsObject)->HasField(TEXT("max"));
					const bool bCenterExtentInput = (*BoundsObject)->HasField(TEXT("center")) || (*BoundsObject)->HasField(TEXT("extent"));
					UeAgentAuthoringDiagnostics::PrefixDocumentFailure(MinMaxError, TEXT("/bounds"), false, true);
					UeAgentAuthoringDiagnostics::PrefixDocumentFailure(CenterExtentError, TEXT("/bounds"), false, true);
					OutError = FFailure::WrapFailure(TEXT("voxel_grid_bounds_shape_invalid"), bMinMaxInput ? MinMaxError : CenterExtentError, TEXT("voxel_grid_input"), TEXT("Provide voxel bounds using finite min/max, or center and positive extent."));
					if (bMinMaxInput && bCenterExtentInput) OutError.Append(CenterExtentError);
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
				OutError = ModelingInputFailure(TEXT("voxel_grid_bounds_space_required"), TEXT("Voxel bounds require an explicit string coordinate space."), *BoundsObject, TEXT("space"), MakeShared<FJsonValueString>(TEXT("world, grid_local or component_local")));
				UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, TEXT("/bounds"), false, true);
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
				if (OutError.TargetPath.IsEmpty() && Session.Actor.IsValid()) OutError.TargetPath = Session.Actor->GetPathName();
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
			UE::UAI::Contract::FDiagnostic& OutError,
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
				OutError = ModelingCondition(TEXT("voxel_grid_expected_revision_required"), Params.IsValid() && Params->HasField(TEXT("expected_revision")) ? TEXT("The managed voxel expected revision is not a finite signed 64-bit integer.") : TEXT("The managed voxel operation is missing its expected revision."), TEXT("/managed_revision_valid"), false, true, Session.Actor.IsValid() ? Session.Actor->GetPathName() : FString());
				OutError.SourceKind = TEXT("command_input"); OutError.Source.JsonPath = TEXT("/expected_revision");
				OutError.ActualEvidence = Params.IsValid() ? Params->Values.FindRef(TEXT("expected_revision")) : nullptr;
				OutError.ExpectedEvidence = MakeShared<FJsonValueString>(TEXT("finite signed 64-bit integer"));
				return false;
			}
			if (Expected != Session.Revision)
			{
				OutError = ModelingCondition(TEXT("voxel_grid_revision_mismatch"), TEXT("The managed voxel state changed after the expected revision was captured."), TEXT("/expected_revision_matches_current"), false, true, Session.Actor.IsValid() ? Session.Actor->GetPathName() : FString());
				OutError.SourceKind = TEXT("command_input"); OutError.Source.JsonPath = TEXT("/expected_revision");
				OutError.ActualEvidence = MakeShared<FJsonValueString>(FString::Printf(TEXT("%lld"), Session.Revision));
				OutError.ExpectedEvidence = MakeShared<FJsonValueString>(FString::Printf(TEXT("%lld"), Expected));
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
			Result->SetStringField(TEXT("geometry_representation"), Session.bSolidGeometry ? TEXT("convex_solid") : TEXT("whole_cells"));
			if (Session.bSolidGeometry)
			{
				Result->SetNumberField(TEXT("solid_piece_count"), Session.SolidPieces.Num());
				Result->SetNumberField(TEXT("volume_cm3"), GridSolid::Volume(Session.SolidPieces));
				FBox Box(ForceInit); for (const auto& Piece : Session.SolidPieces) Box += GridSolid::Bounds(Piece);
				if (Box.IsValid)
				{
					const auto Bounds = MakeShared<FJsonObject>();
					Bounds->SetStringField(TEXT("space"), TEXT("grid_local"));
					Bounds->SetObjectField(TEXT("min"), VectorJson(Box.Min)); Bounds->SetObjectField(TEXT("max"), VectorJson(Box.Max));
					Result->SetObjectField(TEXT("solid_bounds"), Bounds);
				}
			}
			else Result->SetNumberField(TEXT("occupied_cell_count"), Session.Cells.Num());
			Result->SetBoolField(TEXT("dirty"), Session.bDirty);
			Result->SetBoolField(TEXT("committed"), Session.bCommitted);
			Result->SetBoolField(TEXT("state_unknown"), Session.bStateUnknown);
			Result->SetObjectField(
				TEXT("grid_origin"), VectorJson(Session.GridToWorld.GetLocation()));
			Result->SetObjectField(
				TEXT("grid_rotation"), RotatorJson(Session.GridToWorld.Rotator()));
			Result->SetObjectField(TEXT("anchor"), VectorJson(Session.AnchorLocal));
			const TSharedRef<FJsonObject> StateOwner = MakeShared<FJsonObject>();
			StateOwner->SetStringField(TEXT("kind"), TEXT("actor_component"));
			StateOwner->SetStringField(
				TEXT("schema"), GetAuthoringStateSchema());
			StateOwner->SetStringField(
				TEXT("component_name"), GetAuthoringStateComponentName().ToString());
			StateOwner->SetBoolField(
				TEXT("legacy_sidecar_imported"), Session.bLegacySidecarImported);
			Result->SetObjectField(TEXT("authoring_state"), StateOwner);
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
			if (bIncludeCells && MaxCells > 0 && !Session.bSolidGeometry)
			{
				TArray<TSharedPtr<FJsonValue>> Values;
				for (const FIntVector& Cell : SortedCells(Session.Cells))
				{
					if (Values.Num() >= MaxCells) break;
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

		struct FStagedCollisionComponent final
		{
			TWeakObjectPtr<UBoxComponent> Component;
			ECollisionEnabled::Type CollisionEnabled =
				ECollisionEnabled::NoCollision;
			bool bWasTransient = false;
		};

		TArray<FStagedCollisionComponent> StageOldCollisionComponents(
			ADynamicMeshActor& Actor,
			const FString& GridId,
			const TArray<UBoxComponent*>& Keep)
		{
			TSet<const UBoxComponent*> Kept;
			for (const UBoxComponent* Component : Keep) Kept.Add(Component);
			TArray<UActorComponent*> Components;
			Actor.GetComponents(UBoxComponent::StaticClass(), Components);
			TArray<FStagedCollisionComponent> Staged;
			for (UActorComponent* Item : Components)
			{
				UBoxComponent* const Box = Cast<UBoxComponent>(Item);
				if (!Box || Kept.Contains(Box)
					|| !ComponentHasTag(*Box, CollisionTag(GridId)))
				{
					continue;
				}
				FStagedCollisionComponent& Snapshot = Staged.AddDefaulted_GetRef();
				Snapshot.Component = Box;
				Snapshot.CollisionEnabled = Box->GetCollisionEnabled();
				Snapshot.bWasTransient = Box->HasAnyFlags(RF_Transient);
				Box->SetCollisionEnabled(ECollisionEnabled::NoCollision);
				Box->SetFlags(RF_Transient);
			}
			return Staged;
		}

		void RestoreStagedCollisionComponents(
			const TArray<FStagedCollisionComponent>& Staged)
		{
			for (const FStagedCollisionComponent& Snapshot : Staged)
			{
				UBoxComponent* const Box = Snapshot.Component.Get();
				if (!IsValid(Box)) continue;
				if (!Snapshot.bWasTransient) Box->ClearFlags(RF_Transient);
				Box->SetCollisionEnabled(Snapshot.CollisionEnabled);
			}
		}

		void DestroyStagedCollisionComponents(
			const TArray<FStagedCollisionComponent>& Staged)
		{
			for (const FStagedCollisionComponent& Snapshot : Staged)
			{
				if (UBoxComponent* const Box = Snapshot.Component.Get())
				{
					Box->DestroyComponent();
				}
			}
		}

		bool PrepareCollisionComponents(
			ADynamicMeshActor& Actor,
			const FSession& Session,
			const TArray<FVoxelGridCollisionBox>& Boxes,
			const FString& Generation,
			const FName& Profile,
			const ECollisionEnabled::Type Enabled,
			TArray<UBoxComponent*>& OutComponents,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutComponents.Reset();
			USceneComponent* const Root = Actor.GetRootComponent();
			if (!Root)
			{
				OutError = ModelingCondition(TEXT("voxel_grid_actor_root_missing"),
					TEXT("The voxel actor has no root component to attach collision boxes to."),
					TEXT("/root_component_available"), false, true, Actor.GetPathName());
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
					const TSharedRef<FJsonObject> Actual = MakeShared<FJsonObject>();
					Actual->SetNumberField(TEXT("box_index"), Index);
					Actual->SetObjectField(TEXT("min"), IntVectorJson(Box.Min));
					Actual->SetObjectField(TEXT("max_exclusive"), IntVectorJson(Box.MaxExclusive));
					Actual->SetBoolField(TEXT("created"), false);
					OutError = ModelingFact(TEXT("voxel_grid_collision_component_create_failed"),
						TEXT("Unreal could not create the collision box component for this cell range."),
						FString::Printf(TEXT("/collision_components/%d"), Index), MakeShared<FJsonValueObject>(Actual),
						MakeShared<FJsonValueString>(TEXT("created box component")), Actor.GetPathName());
					OutError.CandidateName = Name.ToString();
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
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			for (int32 Index = 0; Index < Components.Num(); ++Index)
			{
				UBoxComponent* const Component = Components[Index];
				Actor.AddInstanceComponent(Component);
				Component->RegisterComponent();
				Component->UpdateOverlaps();
				if (!Component->IsRegistered())
				{
					OutError = ModelingCondition(TEXT("voxel_grid_collision_component_register_failed"),
						TEXT("Unreal did not register the voxel collision box component."),
						FString::Printf(TEXT("/collision_components/%d/registered"), Index), false, true, Actor.GetPathName());
					OutError.CandidateName = Component->GetName();
					return false;
				}
			}
			return true;
		}

		bool TryRangeFromComponent(
			const UBoxComponent& Component,
			const FSession& Session,
			FVoxelGridCollisionBox& OutRange,
			FFailure* OutError = nullptr)
		{
			const bool bRotationValid = Component.GetRelativeRotation().IsNearlyZero(0.001);
			if (!bRotationValid || !Component.GetRelativeScale3D().Equals(FVector::OneVector, 0.001))
			{
				if (OutError)
				{
					*OutError = ModelingFact(TEXT("voxel_grid_collision_transform_invalid"), bRotationValid
						? TEXT("The voxel collision box has a non-unit relative scale.")
						: TEXT("The voxel collision box has a non-zero relative rotation."), bRotationValid
						? TEXT("/relative_scale") : TEXT("/relative_rotation"),
						bRotationValid ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueObject>(GridVectorFact(Component.GetRelativeScale3D())))
						: TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Component.GetRelativeRotation().ToString())),
						MakeShared<FJsonValueString>(bRotationValid ? TEXT("unit scale within 0.001") : TEXT("zero rotation within 0.001")),
						Component.GetOwner() ? Component.GetOwner()->GetPathName() : FString());
					OutError->CandidateName = Component.GetName();
				}
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
			const bool bMinAligned = RawMin.Equals(RoundedMin, 0.001);
			if (!bMinAligned || !RawMax.Equals(RoundedMax, 0.001))
			{
				if (OutError)
				{
					*OutError = ModelingFact(TEXT("voxel_grid_collision_cell_alignment_invalid"),
						TEXT("The voxel collision box boundary is not aligned to integer cells within 0.001."),
						bMinAligned ? TEXT("/cell_range/max_exclusive") : TEXT("/cell_range/min"),
						MakeShared<FJsonValueObject>(GridVectorFact(bMinAligned ? RawMax : RawMin)),
						MakeShared<FJsonValueObject>(GridVectorFact(bMinAligned ? RoundedMax : RoundedMin)),
						Component.GetOwner() ? Component.GetOwner()->GetPathName() : FString());
					OutError->CandidateName = Component.GetName();
				}
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
			const bool bValid = OutRange.IsValid();
			if (!bValid && OutError)
			{
				const TSharedRef<FJsonObject> Actual = MakeShared<FJsonObject>();
				Actual->SetObjectField(TEXT("min"), IntVectorJson(OutRange.Min));
				Actual->SetObjectField(TEXT("max_exclusive"), IntVectorJson(OutRange.MaxExclusive));
				*OutError = ModelingFact(TEXT("voxel_grid_collision_cell_range_invalid"),
					TEXT("The voxel collision box has an empty or reversed cell range."), TEXT("/cell_range"),
					MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(TEXT("max_exclusive greater than min on every axis")),
					Component.GetOwner() ? Component.GetOwner()->GetPathName() : FString());
				OutError->CandidateName = Component.GetName();
			}
			return bValid;
		}

		FVoxelGridAuthoringState AuthoringStateFor(
			const FSession& Session,
			const FString& MaterialPath)
		{
			FVoxelGridAuthoringState State;
			State.GridId = Session.GridId;
			State.CellSize = Session.CellSize;
			State.GridToWorld = Session.GridToWorld;
			State.AnchorLocal = Session.AnchorLocal;
			State.Cells = Session.Cells;
			State.bSolidGeometry = Session.bSolidGeometry;
			State.SolidPieces = Session.SolidPieces;
			State.Revision = Session.Revision;
			State.CollisionGeneration = Session.CollisionGeneration;
			State.CollisionMode = Session.CollisionMode;
			State.MaterialRef = MaterialPath;
			return State;
		}

		void ApplyAuthoringStateToSession(
			const FVoxelGridAuthoringState& State,
			FSession& Session)
		{
			Session.GridId = State.GridId;
			Session.CellSize = State.CellSize;
			Session.GridToWorld = State.GridToWorld;
			Session.AnchorLocal = State.AnchorLocal;
			Session.Cells = State.Cells;
			Session.bSolidGeometry = State.bSolidGeometry;
			Session.SolidPieces = State.SolidPieces;
			Session.Revision = State.Revision;
			Session.CollisionGeneration = State.CollisionGeneration;
			Session.CollisionMode = State.CollisionMode;
			Session.bDirty = false;
			Session.bCommitted = true;
		}

		bool VerifyAppliedState(
			const FSession& Expected,
			ADynamicMeshActor& Actor,
			const FString& MaterialPath,
			const TArray<float>& ExpectedPrimitiveData,
			TSharedRef<FJsonObject>& OutReadback,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutReadback = MakeShared<FJsonObject>();
			UDynamicMeshComponent* const Component = DynamicMeshComponent(&Actor);
			if (!Component || !Component->GetDynamicMesh())
			{
				OutError = ModelingCondition(TEXT("voxel_grid_readback_actor_missing"), Component
					? TEXT("The voxel actor's dynamic mesh component has no mesh for independent readback.")
					: TEXT("The voxel actor has no dynamic mesh component for independent readback."), Component
					? TEXT("/dynamic_mesh_available") : TEXT("/dynamic_mesh_component_available"), false, true, Actor.GetPathName());
				return false;
			}
			if (USceneComponent* const Root = Actor.GetRootComponent())
			{
				Root->UpdateComponentToWorld();
			}
			UUeAgentModelingVoxelGridStateComponent* StateComponent = nullptr;
			if (!TryFindAuthoringStateComponent(
					Actor, StateComponent, OutError)
				|| !StateComponent)
			{
				return false;
			}
			FVoxelGridAuthoringState Persisted;
			if (!TryReadAuthoringState(
					*StateComponent, Persisted, OutError))
			{
				return false;
			}
			FFailure SolidDifference;
			const bool bSolidEquivalent = GridSolid::Equivalent(Persisted.SolidPieces, Expected.SolidPieces, &SolidDifference);
			const bool bAuthoringStateEquivalent =
				Persisted.GridId == Expected.GridId
				&& Persisted.Revision == Expected.Revision
				&& FMath::IsNearlyEqual(
					Persisted.CellSize, Expected.CellSize, 1.e-6)
				&& Persisted.GridToWorld.Equals(Expected.GridToWorld, 0.001)
				&& Persisted.AnchorLocal.Equals(Expected.AnchorLocal, 0.001)
				&& Persisted.CollisionGeneration == Expected.CollisionGeneration
				&& Persisted.CollisionMode == Expected.CollisionMode
				&& Persisted.MaterialRef == MaterialPath
				&& SameCells(Persisted.Cells, Expected.Cells)
				&& Persisted.bSolidGeometry == Expected.bSolidGeometry
				&& bSolidEquivalent;

			int32 VertexCount = 0;
			int32 TriangleCount = 0;
			Component->GetDynamicMesh()->ProcessMesh(
				[&](const FDynamicMesh3& Mesh)
				{
					VertexCount = Mesh.VertexCount();
					TriangleCount = Mesh.TriangleCount();
				});
			FDynamicMesh3 ExpectedMesh;
			int32 FaceCount = 0;
			if (Expected.bSolidGeometry)
			{
				if (!GridSolid::BuildMesh(Expected.SolidPieces, Expected.AnchorLocal, ExpectedMesh, FaceCount, OutError)) return false;
			}
			else FaceCount = BuildExposedFaceMesh(Expected.Cells, Expected.CellSize, Expected.AnchorLocal, ExpectedMesh);
			bool bMeshEquivalent = false;
			FFailure MeshDifference;
			Component->GetDynamicMesh()->ProcessMesh([&](const FDynamicMesh3& Mesh) { bMeshEquivalent = GridSolid::MeshEquivalent(Mesh, ExpectedMesh, &MeshDifference); });

			TSet<FIntVector> Covered;
			bool bCollisionShapeValid = true;
			bool bNoStaleOwnedCollision = true;
			int32 CollisionBoxCount = 0;
			int32 InvalidCollisionBoxCount = 0;
			int32 StaleCollisionBoxCount = 0;
			int32 OverlappingCellCount = 0;
			FIntVector FirstOverlappingCell = FIntVector::ZeroValue;
			FString FirstStaleComponent;
			FFailure FirstCollisionShapeError;
			TArray<UActorComponent*> Components;
			Actor.GetComponents(UBoxComponent::StaticClass(), Components);
			const FString GenerationTag = CollisionGenerationTag(
				Expected.GridId, Expected.CollisionGeneration);
			for (int32 ComponentIndex = 0; ComponentIndex < Components.Num(); ++ComponentIndex)
			{
				UBoxComponent* const Box = Cast<UBoxComponent>(Components[ComponentIndex]);
				if (!Box) continue;
				if (ComponentHasTag(*Box, CollisionTag(Expected.GridId))
					&& !ComponentHasTag(*Box, GenerationTag))
				{
					bNoStaleOwnedCollision = false;
					if (++StaleCollisionBoxCount == 1) FirstStaleComponent = Box->GetName();
				}
				if (!ComponentHasTag(*Box, GenerationTag)) continue;
				++CollisionBoxCount;
				FVoxelGridCollisionBox Range;
				FFailure RangeError;
				if (!TryRangeFromComponent(*Box, Expected, Range, &RangeError))
				{
					bCollisionShapeValid = false;
					if (++InvalidCollisionBoxCount == 1)
					{
						RangeError.PropertyPath = FString::Printf(TEXT("/collision_components/%d"), ComponentIndex) + RangeError.PropertyPath;
						FirstCollisionShapeError = MoveTemp(RangeError);
					}
					continue;
				}
				for (int32 Z = Range.Min.Z; Z < Range.MaxExclusive.Z; ++Z)
				{
					for (int32 Y = Range.Min.Y; Y < Range.MaxExclusive.Y; ++Y)
					{
						for (int32 X = Range.Min.X; X < Range.MaxExclusive.X; ++X)
						{
							const FIntVector Cell(X, Y, Z);
							if (Covered.Contains(Cell))
							{
								bCollisionShapeValid = false;
								if (++OverlappingCellCount == 1) FirstOverlappingCell = Cell;
							}
							Covered.Add(Cell);
						}
					}
				}
			}
			bool bCollisionEquivalent = Expected.CollisionMode == TEXT("none")
				? CollisionBoxCount == 0 && bNoStaleOwnedCollision
				: bNoStaleOwnedCollision
					&& bCollisionShapeValid && SameCells(Covered, Expected.Cells);
			int32 ConvexCount = 0;
			FFailure CollisionDifference;
			if (Expected.bSolidGeometry)
			{
				FKAggregateGeom ExpectedCollision;
				if (Expected.CollisionMode != TEXT("none")) GridSolid::BuildCollision(Expected.SolidPieces, Expected.AnchorLocal, ExpectedCollision);
				const UBodySetup* BodySetup = Component->GetBodySetup();
				ConvexCount = Component->GetSimpleCollisionShapes().ConvexElems.Num();
				bCollisionEquivalent = CollisionBoxCount == 0 && bNoStaleOwnedCollision
					&& GridSolid::CollisionEquivalent(Component->GetSimpleCollisionShapes(), ExpectedCollision, &CollisionDifference)
					&& BodySetup && GridSolid::CollisionEquivalent(BodySetup->AggGeom, ExpectedCollision, &CollisionDifference);
			}
			const UMaterialInterface* const Material = Component->GetMaterial(0);
			const FString ObservedMaterial = Material ? Material->GetPathName() : FString();
			const bool bMaterialEquivalent = ObservedMaterial == MaterialPath;
			const bool bPrimitiveDataEquivalent = Component->GetDefaultCustomPrimitiveData().Data == ExpectedPrimitiveData;
			const bool bTransformEquivalent =
				Actor.GetActorTransform().Equals(ActorTransform(Expected), 0.001);
			const FString ExpectedFolder =
				UeAgentLevelAuthoring::MakeManagedActorFolderPath(
					TEXT("modeling-voxel-grid"));
			const bool bManagedIdentityEquivalent =
				UeAgentLevelAuthoring::GetManagedActorId(Actor) == Expected.GridId
				&& UeAgentLevelAuthoring::GetActorFolderPathText(Actor)
					== ExpectedFolder
				&& Actor.ActorHasTag(
					UeAgentLevelAuthoring::GetManagedActorMarkerTag())
				&& Actor.ActorHasTag(FName(*UeAgentLevelAuthoring::
					MakeManagedActorTag(Expected.GridId)));

			OutReadback->SetBoolField(
				TEXT("authoring_state_equivalent"), bAuthoringStateEquivalent);
			OutReadback->SetBoolField(TEXT("mesh_equivalent"), bMeshEquivalent);
			OutReadback->SetBoolField(
				TEXT("collision_exact_cover"), bCollisionEquivalent);
			OutReadback->SetBoolField(
				TEXT("no_stale_owned_collision"), bNoStaleOwnedCollision);
			OutReadback->SetBoolField(
				TEXT("material_equivalent"), bMaterialEquivalent);
			OutReadback->SetBoolField(TEXT("custom_primitive_data_equivalent"), bPrimitiveDataEquivalent);
			OutReadback->SetBoolField(
				TEXT("transform_equivalent"), bTransformEquivalent);
			OutReadback->SetStringField(
				TEXT("expected_actor_transform"),
				ActorTransform(Expected).ToHumanReadableString());
			OutReadback->SetStringField(
				TEXT("observed_actor_transform"),
				Actor.GetActorTransform().ToHumanReadableString());
			OutReadback->SetBoolField(
				TEXT("managed_identity_equivalent"), bManagedIdentityEquivalent);
			OutReadback->SetNumberField(TEXT("vertex_count"), VertexCount);
			OutReadback->SetNumberField(TEXT("triangle_count"), TriangleCount);
			OutReadback->SetNumberField(TEXT("collision_convex_count"), ConvexCount);
			OutReadback->SetNumberField(TEXT("exposed_face_count"), FaceCount);
			OutReadback->SetNumberField(
				TEXT("collision_box_count"), CollisionBoxCount);
			OutReadback->SetNumberField(
				TEXT("covered_cell_count"), Covered.Num());
			const bool bEquivalent = bAuthoringStateEquivalent
				&& bMeshEquivalent
				&& bCollisionEquivalent
				&& bMaterialEquivalent
				&& bPrimitiveDataEquivalent
				&& bTransformEquivalent
				&& bManagedIdentityEquivalent;
			OutReadback->SetBoolField(TEXT("equivalent"), bEquivalent);
			if (!bEquivalent)
			{
				OutError = ModelingCondition(TEXT("voxel_grid_independent_readback_mismatch"),
					TEXT("Independent voxel readback differs from the intended state; the field diagnostics identify the differences."),
					TEXT("/equivalent"), false, true, Actor.GetPathName());
				const auto AddFact = [&](const FString& Field, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Wanted, const FString& Reason)
				{
					OutError.Diagnostics.Add(ModelingFact(TEXT("voxel_grid_readback_difference"), Reason, Field,
						MoveTemp(Actual), MoveTemp(Wanted), Actor.GetPathName()));
				};
				const auto AddPrivateDifference = [&](const FString& Field, const FString& Actual, const FString& Wanted)
				{
					if (Actual == Wanted) return;
					FFailure Difference = ModelingCondition(TEXT("voxel_grid_readback_identity_mismatch"),
						TEXT("The managed voxel state field differs from the intended state."), Field, false, true, Actor.GetPathName());
					Difference.ActualEvidence = MakeShared<FJsonValueString>(Actual);
					Difference.ExpectedEvidence = MakeShared<FJsonValueString>(Wanted);
					OutError.Diagnostics.Add(MoveTemp(Difference));
				};
				const auto AddVectorDifferences = [&](const FString& Field, const FVector& Actual, const FVector& Wanted)
				{
					const TCHAR* Axes[] = { TEXT("x"), TEXT("y"), TEXT("z") };
					for (int32 Axis = 0; Axis < 3; ++Axis)
					{
						if (!FMath::IsNearlyEqual(Actual[Axis], Wanted[Axis], 0.001))
							AddFact(Field + TEXT("/") + Axes[Axis], GridNumberFact(Actual[Axis]), GridNumberFact(Wanted[Axis]),
								TEXT("The coordinate differs from the intended value by more than 0.001."));
					}
				};
				const auto AddTransformDifferences = [&](const FString& Field, const FTransform& Actual, const FTransform& Wanted)
				{
					AddVectorDifferences(Field + TEXT("/translation"), Actual.GetTranslation(), Wanted.GetTranslation());
					AddVectorDifferences(Field + TEXT("/scale"), Actual.GetScale3D(), Wanted.GetScale3D());
					if (!Actual.GetRotation().Equals(Wanted.GetRotation(), 0.001))
						AddFact(Field + TEXT("/rotation"), MakeShared<FJsonValueString>(Actual.GetRotation().ToString()),
							MakeShared<FJsonValueString>(Wanted.GetRotation().ToString()), TEXT("The rotation quaternion differs from the intended value beyond tolerance 0.001."));
				};
				const auto AddCellDifferences = [&](const FString& Field, const TSet<FIntVector>& Actual, const TSet<FIntVector>& Wanted)
				{
					int32 MissingCount = 0;
					int32 UnexpectedCount = 0;
					FIntVector FirstMissing = FIntVector::ZeroValue;
					FIntVector FirstUnexpected = FIntVector::ZeroValue;
					const auto Before = [](const FIntVector& Left, const FIntVector& Right)
					{
						return Left.X != Right.X ? Left.X < Right.X : Left.Y != Right.Y ? Left.Y < Right.Y : Left.Z < Right.Z;
					};
					for (const FIntVector& Cell : Wanted)
						if (!Actual.Contains(Cell))
						{
							if (++MissingCount == 1 || Before(Cell, FirstMissing)) FirstMissing = Cell;
						}
					for (const FIntVector& Cell : Actual)
						if (!Wanted.Contains(Cell))
						{
							if (++UnexpectedCount == 1 || Before(Cell, FirstUnexpected)) FirstUnexpected = Cell;
						}
					if (MissingCount > 0)
						AddFact(Field + TEXT("/missing_cell"), MakeShared<FJsonValueNull>(), MakeShared<FJsonValueObject>(IntVectorJson(FirstMissing)),
							FString::Printf(TEXT("%d expected cells are missing; this is the first missing coordinate."), MissingCount));
					if (UnexpectedCount > 0)
						AddFact(Field + TEXT("/unexpected_cell"), MakeShared<FJsonValueObject>(IntVectorJson(FirstUnexpected)), MakeShared<FJsonValueNull>(),
							FString::Printf(TEXT("%d unexpected cells were read back; this is the first unexpected coordinate."), UnexpectedCount));
				};
				if (!bAuthoringStateEquivalent)
				{
					AddPrivateDifference(TEXT("/authoring_state/grid_identity_matches"), Persisted.GridId, Expected.GridId);
					AddPrivateDifference(TEXT("/authoring_state/revision_matches"), FString::Printf(TEXT("%lld"), Persisted.Revision), FString::Printf(TEXT("%lld"), Expected.Revision));
					AddPrivateDifference(TEXT("/authoring_state/collision_generation_matches"), Persisted.CollisionGeneration, Expected.CollisionGeneration);
					if (!FMath::IsNearlyEqual(Persisted.CellSize, Expected.CellSize, 1.e-6))
						AddFact(TEXT("/authoring_state/cell_size"), GridNumberFact(Persisted.CellSize), GridNumberFact(Expected.CellSize), TEXT("Cell size differs from the intended value beyond tolerance 0.000001."));
					AddTransformDifferences(TEXT("/authoring_state/grid_to_world"), Persisted.GridToWorld, Expected.GridToWorld);
					AddVectorDifferences(TEXT("/authoring_state/anchor"), Persisted.AnchorLocal, Expected.AnchorLocal);
					if (Persisted.CollisionMode != Expected.CollisionMode)
						AddFact(TEXT("/authoring_state/collision_mode"), MakeShared<FJsonValueString>(Persisted.CollisionMode), MakeShared<FJsonValueString>(Expected.CollisionMode), TEXT("Serialized collision mode differs from the intended mode."));
					if (Persisted.MaterialRef != MaterialPath)
						AddFact(TEXT("/authoring_state/material_ref"), MakeShared<FJsonValueString>(Persisted.MaterialRef), MakeShared<FJsonValueString>(MaterialPath), TEXT("Serialized material reference differs from the intended material."));
					AddCellDifferences(TEXT("/authoring_state/cells"), Persisted.Cells, Expected.Cells);
					if (Persisted.bSolidGeometry != Expected.bSolidGeometry)
						AddFact(TEXT("/authoring_state/solid_geometry"), MakeShared<FJsonValueBoolean>(Persisted.bSolidGeometry), MakeShared<FJsonValueBoolean>(Expected.bSolidGeometry), TEXT("The geometry representation differs from the intended representation."));
					if (!SolidDifference.IsEmpty()) OutError.Diagnostics.Add(SolidDifference);
				}
				if (!bMeshEquivalent)
				{
					if (!MeshDifference.IsEmpty()) OutError.Diagnostics.Add(MeshDifference);
				}
				if (!bCollisionEquivalent)
				{
					if (!CollisionDifference.IsEmpty()) OutError.Diagnostics.Add(CollisionDifference);
					if (!bNoStaleOwnedCollision)
					{
						AddFact(TEXT("/collision/stale_component_count"), MakeShared<FJsonValueNumber>(StaleCollisionBoxCount), MakeShared<FJsonValueNumber>(0), TEXT("Collision components from an older managed generation remain on the actor."));
						OutError.Diagnostics.Last().CandidateName = FirstStaleComponent;
					}
					if (Expected.CollisionMode == TEXT("none"))
					{
						if (CollisionBoxCount != 0) AddFact(TEXT("/collision/box_count"), MakeShared<FJsonValueNumber>(CollisionBoxCount), MakeShared<FJsonValueNumber>(0), TEXT("Collision boxes remain although collision mode is none."));
					}
					else
					{
						if (FirstCollisionShapeError.IsValid())
						{
							FirstCollisionShapeError.PublicMessage += FString::Printf(TEXT(" %d collision boxes are invalid; this is the first."), InvalidCollisionBoxCount);
							OutError.Diagnostics.Add(MoveTemp(FirstCollisionShapeError));
						}
						if (OverlappingCellCount > 0) AddFact(TEXT("/collision/overlapping_cell"), MakeShared<FJsonValueObject>(IntVectorJson(FirstOverlappingCell)),
							MakeShared<FJsonValueString>(TEXT("each cell covered once")), FString::Printf(TEXT("Collision boxes overlap at %d repeated cell occurrences; this is the first coordinate."), OverlappingCellCount));
						AddCellDifferences(TEXT("/collision/covered_cells"), Covered, Expected.Cells);
					}
				}
				if (!bMaterialEquivalent) AddFact(TEXT("/material_ref"), MakeShared<FJsonValueString>(ObservedMaterial), MakeShared<FJsonValueString>(MaterialPath), TEXT("The mesh component material differs from the intended material."));
				if (!bPrimitiveDataEquivalent) AddFact(TEXT("/custom_primitive_data"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("Persisted per-object color or grid origin differs from the intended component defaults."));
				if (!bTransformEquivalent) AddTransformDifferences(TEXT("/actor_transform"), Actor.GetActorTransform(), ActorTransform(Expected));
				if (!bManagedIdentityEquivalent)
				{
					AddPrivateDifference(TEXT("/managed_actor/grid_identity_matches"), UeAgentLevelAuthoring::GetManagedActorId(Actor), Expected.GridId);
					const FString ObservedFolder = UeAgentLevelAuthoring::GetActorFolderPathText(Actor);
					if (ObservedFolder != ExpectedFolder) AddFact(TEXT("/managed_actor/folder"), MakeShared<FJsonValueString>(ObservedFolder), MakeShared<FJsonValueString>(ExpectedFolder), TEXT("The actor is outside the managed voxel folder."));
					if (!Actor.ActorHasTag(UeAgentLevelAuthoring::GetManagedActorMarkerTag())) AddFact(TEXT("/managed_actor/marker_present"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("The actor lacks the managed actor marker."));
					if (!Actor.ActorHasTag(FName(*UeAgentLevelAuthoring::MakeManagedActorTag(Expected.GridId)))) AddFact(TEXT("/managed_actor/grid_tag_present"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("The actor lacks the expected managed voxel grid tag."));
				}
			}
			return bEquivalent;
		}

		class FProcessRegistry final
		{
		public:
			bool ExecuteSession(
				const FUeAgentCommandRequest& Request,
				TSharedPtr<FJsonObject>& OutPayload,
				FFailure& OutFailure,
				const bool bBeginCommand = false)
			{
				OutPayload.Reset();
				OutFailure = {};
				FScopeLock Lock(&Mutex);
				UWorld* const World = EditorWorld(OutFailure);
				if (!World) return false;
				FString Action;
				const TSharedPtr<FJsonObject>& Params = Request.GetParams();
				if (bBeginCommand)
				{
					Action = TEXT("begin");
				}
				else if (!Params.IsValid()
					|| !Params->TryGetStringField(TEXT("action"), Action))
				{
					SetFailure(OutFailure, ModelingInputFailure(TEXT("voxel_grid_action_required"),
						TEXT("Specify a voxel session action as a string."), Params, TEXT("action"),
						MakeShared<FJsonValueString>(TEXT("begin, read, add_bounds, remove_bounds, close, or cancel"))),
						TEXT("not_started"), true, false, UE::UAI::Contract::FDiagnostic());
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
					SetFailure(OutFailure, SessionInputFailure(Params), TEXT("not_started"), true, false, UE::UAI::Contract::FDiagnostic());
					return false;
				}
				FSession* const Session = Sessions.Find(SessionId);
				if (!Session || !Session->Actor.IsValid())
				{
					SetFailure(OutFailure, SessionLookupFailure(SessionId, Session), TEXT("not_started"), true, false, UE::UAI::Contract::FDiagnostic());
					return false;
				}
				if (Action == TEXT("read"))
				{
					UE::UAI::Contract::FDiagnostic Error;
					if (!CheckRevision(Params, *Session, Error, false))
					{
						SetFailure(OutFailure, Error, TEXT("not_started"), true, false, UE::UAI::Contract::FDiagnostic());
						return false;
					}
					OutPayload = SuccessPayload(
						Request, Action, SessionObservation(*Session, Params));
					return true;
				}
				if (Session->bStateUnknown && Action != TEXT("cancel"))
				{
					SetFailure(OutFailure, ModelingCondition(TEXT("voxel_grid_session_state_unknown"),
						TEXT("The voxel session state is unknown; cancel and reconcile the persisted actor before continuing."),
						TEXT("/state_unknown"), true, false, Session->Actor->GetPathName()),
						TEXT("state_unknown"), false, true, UE::UAI::Contract::FDiagnostic());
					return false;
				}
				if (Action == TEXT("add_bounds")
					|| Action == TEXT("remove_bounds"))
				{
					return MutateBounds(
						Request, Action, *Session, OutPayload, OutFailure);
				}
				if (Action == TEXT("add_shape") || Action == TEXT("remove_shape"))
				{
					return MutateShape(Request, Action, *Session, OutPayload, OutFailure);
				}
				if (Action == TEXT("close") || Action == TEXT("cancel"))
				{
					return Close(
						Request, Action, *Session, OutPayload, OutFailure);
				}
				SetFailure(OutFailure, ModelingInputFailure(TEXT("voxel_grid_action_unknown"),
					TEXT("The voxel session action is not supported."), Params, TEXT("action"),
					MakeShared<FJsonValueString>(TEXT("begin, read, add_bounds, remove_bounds, close, or cancel"))),
					TEXT("not_started"), true, false, UE::UAI::Contract::FDiagnostic());
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
					SetFailure(OutFailure, SessionInputFailure(Params), TEXT("not_started"), true, false, UE::UAI::Contract::FDiagnostic());
					return false;
				}
				FSession* const Session = Sessions.Find(SessionId);
				UE::UAI::Contract::FDiagnostic Error;
				if (!Session || !Session->Actor.IsValid())
				{
					SetFailure(OutFailure, SessionLookupFailure(SessionId, Session), TEXT("not_started"), true, false, UE::UAI::Contract::FDiagnostic());
					return false;
				}
				if (Session->bStateUnknown)
				{
					SetFailure(OutFailure, ModelingCondition(TEXT("voxel_grid_session_state_unknown"),
						TEXT("The voxel session state is unknown; cancel and reconcile the persisted actor before continuing."),
						TEXT("/state_unknown"), true, false, Session->Actor->GetPathName()),
						TEXT("state_unknown"), false, true, UE::UAI::Contract::FDiagnostic());
					return false;
				}
				if (!CheckRevision(Params, *Session, Error))
				{
					SetFailure(OutFailure, Error, TEXT("not_started"), true, false, UE::UAI::Contract::FDiagnostic());
					return false;
				}
				ADynamicMeshActor* const Actor = Session->Actor.Get();
				UDynamicMeshComponent* const Component = DynamicMeshComponent(Actor);
				if (!Actor || !Component || !Component->GetDynamicMesh())
				{
					const FString MissingField = !Actor ? TEXT("/actor_available")
						: !Component ? TEXT("/dynamic_mesh_component_available") : TEXT("/dynamic_mesh_available");
					Error = ModelingCondition(TEXT("voxel_grid_dynamic_mesh_component_required"), !Actor
						? TEXT("The voxel actor is no longer available.")
						: !Component ? TEXT("The voxel actor has no dynamic mesh component.")
						: TEXT("The voxel actor's dynamic mesh component has no mesh."),
						MissingField, false, true, Actor ? Actor->GetPathName() : FString());
					SetFailure(OutFailure, Error, TEXT("not_started"), true, false, UE::UAI::Contract::FDiagnostic());
					return false;
				}

				FString MeshBuild = TEXT("exposed_faces");
				Params->TryGetStringField(TEXT("mesh_build"), MeshBuild);
				MeshBuild.ToLowerInline();
				if (MeshBuild != TEXT("exposed_faces"))
				{
					SetFailure(OutFailure, ModelingInputFailure(TEXT("voxel_grid_mesh_build_unsupported"),
						TEXT("Only exposed-face mesh generation is supported for voxel grids."), Params, TEXT("mesh_build"),
						MakeShared<FJsonValueString>(TEXT("exposed_faces"))), TEXT("not_started"), true, false, UE::UAI::Contract::FDiagnostic());
					return false;
				}

				FString CollisionMode = Session->bSolidGeometry ? TEXT("exact_convex") : TEXT("exact_box_components");
				FString CollisionProfile = TEXT("BlockAll");
				FString CollisionEnabled = TEXT("query_and_physics");
				const TSharedPtr<FJsonObject>* Collision = nullptr;
				if (Params->TryGetObjectField(TEXT("collision"), Collision)
					&& Collision && Collision->IsValid())
				{
					(*Collision)->TryGetStringField(TEXT("mode"), CollisionMode);
					(*Collision)->TryGetStringField(
						TEXT("profile_name"), CollisionProfile);
					(*Collision)->TryGetStringField(
						TEXT("enabled"), CollisionEnabled);
				}
				CollisionMode.TrimStartAndEndInline();
				CollisionMode.ToLowerInline();
				CollisionEnabled.TrimStartAndEndInline();
				CollisionEnabled.ToLowerInline();
				if (CollisionMode == TEXT("disabled")) CollisionMode = TEXT("none");
				if (CollisionMode != TEXT("none")
					&& CollisionMode != TEXT("exact_box_components") && CollisionMode != TEXT("exact_convex"))
				{
					Error = ModelingInputFailure(TEXT("voxel_grid_collision_mode_unsupported"), TEXT("The voxel collision mode is not supported."),
						Collision ? *Collision : nullptr, TEXT("mode"), MakeShared<FJsonValueString>(TEXT("none, disabled, exact_convex, or exact_box_components (whole cells only)")));
					UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error, TEXT("/collision"), false, true);
					Error.TargetPath = Actor->GetPathName();
					SetFailure(OutFailure, Error, TEXT("not_started"), true, false, UE::UAI::Contract::FDiagnostic());
					return false;
				}
				if (CollisionEnabled != TEXT("none")
					&& CollisionEnabled != TEXT("no_collision")
					&& CollisionEnabled != TEXT("query_only")
					&& CollisionEnabled != TEXT("physics_only")
					&& CollisionEnabled != TEXT("query_and_physics"))
				{
					Error = ModelingInputFailure(TEXT("voxel_grid_collision_enabled_unsupported"), TEXT("The voxel collision-enabled setting is not supported."),
						Collision ? *Collision : nullptr, TEXT("enabled"), MakeShared<FJsonValueString>(TEXT("none, no_collision, query_only, physics_only, or query_and_physics")));
					UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error, TEXT("/collision"), false, true);
					Error.TargetPath = Actor->GetPathName();
					SetFailure(OutFailure, Error, TEXT("not_started"), true, false, UE::UAI::Contract::FDiagnostic());
					return false;
				}
				if (CollisionProfile.TrimStartAndEnd().IsEmpty())
				{
					Error = ModelingInputFailure(TEXT("voxel_grid_collision_profile_invalid"), TEXT("The voxel collision profile name must not be empty."),
						Collision ? *Collision : nullptr, TEXT("profile_name"), MakeShared<FJsonValueString>(TEXT("non-empty collision profile name")));
					UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error, TEXT("/collision"), false, true);
					Error.TargetPath = Actor->GetPathName();
					SetFailure(OutFailure, Error, TEXT("not_started"), true, false, UE::UAI::Contract::FDiagnostic());
					return false;
				}
				TArray<FVoxelGridCollisionBox> Boxes;
				if (Session->bSolidGeometry && CollisionMode == TEXT("exact_box_components"))
				{
					Error = ModelingInputFailure(TEXT("voxel_grid_shape_requires_convex_collision"),
						TEXT("Exact box collision cannot represent sloped or cut solids. Use exact_convex, none, or omit collision to select exact collision automatically."),
						Collision ? *Collision : nullptr, TEXT("mode"), MakeShared<FJsonValueString>(TEXT("exact_convex or none")));
					UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error, TEXT("/collision"), false, true);
					SetFailure(OutFailure, Error, TEXT("not_started"), true, false, {}); return false;
				}
				if (CollisionMode == TEXT("exact_convex") && !Session->bSolidGeometry) PromoteToSolid(*Session);
				if (CollisionMode == TEXT("exact_box_components"))
				{
					Boxes = BuildExactCoverBoxes(Session->Cells);
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
						Error = ModelingInputFailure(TEXT("voxel_grid_material_not_found"),
							TEXT("The requested material could not be loaded as a MaterialInterface asset."),
							Params, TEXT("material_ref"), MakeShared<FJsonValueString>(TEXT("loadable MaterialInterface asset reference")));
						Error.TargetPath = RequestedMaterial;
						SetFailure(OutFailure, Error, TEXT("not_started"), true, false, UE::UAI::Contract::FDiagnostic());
						return false;
					}
					MaterialPath = Material->GetPathName();
				}
				else if (Session->bCreatedActor && !Session->bCommitted)
				{
					Material = UE::UAI::ModelingMetricMaterial::Load();
					if (!Material)
					{
						Error = ModelingCondition(TEXT("modeling_metric_material_missing"), TEXT("Install the UAI plugin Content directory before creating metric-grid geometry."), TEXT("/metric_material_available"), false, true);
						SetFailure(OutFailure, Error, TEXT("not_started"), true, false, {}); return false;
					}
					MaterialPath = Material->GetPathName();
				}
				TOptional<FLinearColor> MetricColor;
				if (!UE::UAI::ModelingMetricMaterial::TryReadColor(Params, MetricColor)
					|| (MetricColor.IsSet() && !UE::UAI::ModelingMetricMaterial::IsMetric(Material)))
				{
					Error = ModelingInputFailure(TEXT("modeling_color_invalid"), TEXT("color requires three linear RGB numbers in [0,1] and the built-in metric material."), Params, TEXT("color"), MakeShared<FJsonValueString>(TEXT("linear RGB [r,g,b] for the UAI metric material")));
					SetFailure(OutFailure, Error, TEXT("not_started"), true, false, {}); return false;
				}

				FDynamicMesh3 NewMesh;
				int32 FaceCount = 0;
				if (Session->bSolidGeometry)
				{
					if (!GridSolid::BuildMesh(Session->SolidPieces, Session->AnchorLocal, NewMesh, FaceCount, Error))
					{ SetFailure(OutFailure, Error, TEXT("not_started"), true, false, {}); return false; }
				}
				else FaceCount = BuildExposedFaceMesh(Session->Cells, Session->CellSize, Session->AnchorLocal, NewMesh);
				const int32 NewVertexCount = NewMesh.VertexCount(), NewTriangleCount = NewMesh.TriangleCount();
				const FBox MetricLocalBounds(NewMesh.GetBounds().Min, NewMesh.GetBounds().Max);
				FKAggregateGeom NewSimpleCollision;
				if (CollisionMode == TEXT("exact_convex")) GridSolid::BuildCollision(Session->SolidPieces, Session->AnchorLocal, NewSimpleCollision);
				const FKAggregateGeom OldSimpleCollision = Component->GetSimpleCollisionShapes();
				const auto OldCollisionType = Component->CollisionType;
				const bool bOldAsyncCooking = Component->bUseAsyncCooking;
				const bool bOldComplexCollision = Component->bEnableComplexCollision;
				const FName OldCollisionProfile = Component->GetCollisionProfileName();
				const auto RestoreSimpleCollision = [&]() {
					Component->CollisionType = OldCollisionType;
					Component->bEnableComplexCollision = bOldComplexCollision;
					Component->bUseAsyncCooking = false;
					Component->SetCollisionProfileName(OldCollisionProfile);
					Component->SetSimpleCollisionShapes(OldSimpleCollision, true);
					Component->bUseAsyncCooking = bOldAsyncCooking;
				};
				FDynamicMesh3 OldMesh;
				Component->GetDynamicMesh()->ProcessMesh(
					[&](const FDynamicMesh3& Mesh) { OldMesh = Mesh; });
				UMaterialInterface* const OldMaterial = Component->GetMaterial(0);
				const FCustomPrimitiveData OldPrimitiveData = Component->GetDefaultCustomPrimitiveData();
				const auto RestoreMaterial = [&]() {
					Component->SetMaterial(0, OldMaterial);
					UE::UAI::ModelingMetricMaterial::RestoreDefaults(*Component, OldPrimitiveData);
				};
				const ECollisionEnabled::Type OldCollisionEnabled =
					Component->GetCollisionEnabled();
				const FTransform OldActorTransform = Actor->GetActorTransform();
				const TArray<FName> OldActorTags = Actor->Tags;
				const FName OldActorFolder = Actor->GetFolderPath();
				if (!Session->DurableSession.IsValid())
				{
					UWorld* const ActorWorld = Actor->GetWorld();
					if (!ActorWorld
						|| !TryPrepareDurability(
							Request,
							*ActorWorld,
							Actor,
							Session->bCreatedActor && !Session->bCommitted,
							*Session,
							Error))
					{
						SetFailure(OutFailure, ActorWorld ? Error : ModelingCondition(TEXT("voxel_grid_actor_world_missing"),
							TEXT("The voxel actor is not attached to a world; its durability session cannot be prepared."),
							TEXT("/actor_world_available"), false, true, Actor->GetPathName()),
							TEXT("not_started"), true, false, UE::UAI::Contract::FDiagnostic());
						return false;
					}
				}
				if (!TryEnlistActorPackage(*Session, *Actor, Error))
				{
					const auto EnlistError = Error;
					UE::UAI::Contract::FDiagnostic RollbackError;
					const bool bRolledBack = TryRollbackDurability(
						*Session, true, RollbackError);
					Session->bStateUnknown = !bRolledBack;
					SetFailure(OutFailure, EnlistError, bRolledBack ? FString(TEXT("rolled_back"))
							: FString(TEXT("state_unknown")), bRolledBack, true, RollbackError);
					return false;
				}
				if (!UeAgentLevelAuthoring::TryBindManagedActorOwnership(
						*Actor,
						Session->GridId,
						TEXT("modeling-voxel-grid"),
						Error))
				{
					SetFailure(OutFailure, Error, TEXT("not_started"), true, false, UE::UAI::Contract::FDiagnostic());
					return false;
				}
				UUeAgentModelingVoxelGridStateComponent* StateComponent = nullptr;
				TOptional<FVoxelGridAuthoringState> PreviousAuthoringState;
				UE::UAI::Contract::FDiagnostic StateError;
				if (TryFindAuthoringStateComponent(
						*Actor, StateComponent, StateError))
				{
					FVoxelGridAuthoringState ExistingState;
					if (!TryReadAuthoringState(
							*StateComponent,
							ExistingState,
							StateError))
					{
						SetFailure(OutFailure, StateError, TEXT("not_started"), true, false, UE::UAI::Contract::FDiagnostic());
						return false;
					}
					PreviousAuthoringState = MoveTemp(ExistingState);
				}
				else if (StateError.Code != TEXT("voxel_grid_authoring_state_missing"))
				{
					SetFailure(OutFailure, StateError, TEXT("not_started"), true, false, UE::UAI::Contract::FDiagnostic());
					return false;
				}

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
					SetFailure(OutFailure, Error, TEXT("not_started"), true, false, UE::UAI::Contract::FDiagnostic());
					return false;
				}

				FSession Intended = *Session;
				Intended.CollisionGeneration = Generation;
				Intended.CollisionMode = CollisionMode;

				if (!ActivateCollisionComponents(
					*Actor, NewCollisionComponents, Error))
				{
					DestroyComponents(NewCollisionComponents);
					SetFailure(OutFailure, Error, TEXT("rolled_back"), true, true, UE::UAI::Contract::FDiagnostic());
					return false;
				}

				Component->Modify();
				Component->SetMesh(MoveTemp(NewMesh));
				Component->SetMaterial(0, Material);
				if (NewVertexCount > 0) UE::UAI::ModelingMetricMaterial::Initialize(*Component, MetricLocalBounds, MetricColor);
				Component->bUseAsyncCooking = false;
				Component->bEnableComplexCollision = false;
				Component->CollisionType = CTF_UseSimpleAsComplex;
				Component->SetCollisionProfileName(FName(*CollisionProfile));
				Component->SetSimpleCollisionShapes(NewSimpleCollision, true);
				Component->SetCollisionEnabled(CollisionMode == TEXT("exact_convex") ? Enabled : ECollisionEnabled::NoCollision);
				if (CollisionMode == TEXT("exact_convex"))
				{
					const UBodySetup* Body = Component->GetBodySetup();
					int32 FailedPiece = INDEX_NONE;
					if (Body) for (int32 I = 0; I < Body->AggGeom.ConvexElems.Num(); ++I)
						if (!Body->AggGeom.ConvexElems[I].GetChaosConvexMesh()) { FailedPiece = I; break; }
					if (!Body || FailedPiece != INDEX_NONE || !GridSolid::CollisionEquivalent(Body->AggGeom, NewSimpleCollision))
					{
						Error = ModelingFact(TEXT("voxel_grid_collision_cook_failed"),
							TEXT("Unreal did not produce the intended cooked convex collision; the previous mesh and collision have been restored."),
							FString::Printf(TEXT("/collision/convex_pieces/%d"), FailedPiece),
							MakeShared<FJsonValueString>(Body ? TEXT("missing or different cooked convex") : TEXT("no BodySetup")),
							MakeShared<FJsonValueString>(TEXT("cooked convex matching the solid piece")), Actor->GetPathName());
						Component->SetMesh(MoveTemp(OldMesh)); RestoreMaterial();
						RestoreSimpleCollision(); Component->SetCollisionEnabled(OldCollisionEnabled);
						DestroyComponents(NewCollisionComponents);
						SetFailure(OutFailure, Error, TEXT("rolled_back"), true, true, {}); return false;
					}
				}
				Component->MarkRenderStateDirty();
				Actor->Modify();
				Actor->SetActorTransform(
					ActorTransform(*Session), false, nullptr,
					ETeleportType::TeleportPhysics);
				Actor->Tags.AddUnique(FName(*ActorTag(Session->GridId)));
				Intended.Actor = Actor;
				bool bCreatedStateComponent = false;
				if (!StateComponent)
				{
					if (!TryCreateAuthoringStateComponent(
							*Actor, StateComponent, Error))
					{
						Component->SetMesh(MoveTemp(OldMesh));
						RestoreMaterial();
					RestoreSimpleCollision();
						Component->SetCollisionEnabled(OldCollisionEnabled);
						Component->MarkRenderStateDirty();
						Actor->SetActorTransform(
							OldActorTransform, false, nullptr,
							ETeleportType::TeleportPhysics);
						Actor->Tags = OldActorTags;
						Actor->SetFolderPath(OldActorFolder);
						DestroyComponents(NewCollisionComponents);
						SetFailure(OutFailure, Error, TEXT("rolled_back"), true, true, UE::UAI::Contract::FDiagnostic());
						return false;
					}
					bCreatedStateComponent = true;
				}
				if (!TryWriteAuthoringState(
						*StateComponent,
						AuthoringStateFor(Intended, MaterialPath),
						Error))
				{
					Component->SetMesh(MoveTemp(OldMesh));
					RestoreMaterial();
					RestoreSimpleCollision();
					Component->SetCollisionEnabled(OldCollisionEnabled);
					Component->MarkRenderStateDirty();
					Actor->SetActorTransform(
						OldActorTransform, false, nullptr,
						ETeleportType::TeleportPhysics);
					Actor->Tags = OldActorTags;
					Actor->SetFolderPath(OldActorFolder);
					DestroyComponents(NewCollisionComponents);
					if (bCreatedStateComponent)
					{
						StateComponent->DestroyComponent();
					}
					SetFailure(OutFailure, Error, TEXT("rolled_back"), true, true, UE::UAI::Contract::FDiagnostic());
					return false;
				}
				const TArray<FStagedCollisionComponent> StagedOldCollision =
					StageOldCollisionComponents(
						*Actor, Session->GridId, NewCollisionComponents);
				Actor->MarkPackageDirty();
				TSharedRef<FJsonObject> PersistedReadback =
					MakeShared<FJsonObject>();
				if (!TryPersistAndVerify(
						*Session,
						Intended,
						MaterialPath,
						PersistedReadback,
						Error))
				{
					const auto PersistError = Error;
					Component->SetMesh(MoveTemp(OldMesh));
					RestoreMaterial();
					RestoreSimpleCollision();
					Component->SetCollisionEnabled(OldCollisionEnabled);
					Component->MarkRenderStateDirty();
					Actor->SetActorTransform(
						OldActorTransform, false, nullptr,
						ETeleportType::TeleportPhysics);
					Actor->Tags = OldActorTags;
					Actor->SetFolderPath(OldActorFolder);
					DestroyComponents(NewCollisionComponents);
					RestoreStagedCollisionComponents(StagedOldCollision);
					bool bStateRestored = true;
					if (PreviousAuthoringState.IsSet())
					{
						bStateRestored = TryWriteAuthoringState(
							*StateComponent,
							PreviousAuthoringState.GetValue(),
							StateError);
					}
					else if (bCreatedStateComponent)
					{
						StateComponent->DestroyComponent();
					}
					UE::UAI::Contract::FDiagnostic RollbackError;
					const bool bRolledBack = TryRollbackDurability(
						*Session, bStateRestored, RollbackError);
					Session->bStateUnknown = !bRolledBack;
					SetFailure(OutFailure, PersistError, bRolledBack ? FString(TEXT("rolled_back"))
							: FString(TEXT("state_unknown")), bRolledBack, true, RollbackError);
					return false;
				}
				DestroyStagedCollisionComponents(StagedOldCollision);
				if (UPackage* ActorPackage = Actor->GetOutermost())
				{
					ActorPackage->SetDirtyFlag(false);
				}
				if (UWorld* ActorWorld = Actor->GetWorld())
				{
					if (UPackage* WorldPackage = ActorWorld->GetOutermost())
					{
						WorldPackage->SetDirtyFlag(false);
					}
				}
				Session->CollisionGeneration = Generation;
				Session->CollisionMode = CollisionMode;
				Session->bDirty = false;
				Session->bCommitted = true;
				Session->bStateUnknown = false;
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
				if (Session->bSolidGeometry) Payload->SetNumberField(TEXT("volume_cm3"), GridSolid::Volume(Session->SolidPieces));
				else Payload->SetNumberField(TEXT("occupied_cell_count"), Session->Cells.Num());
				Payload->SetBoolField(TEXT("session_closed"), !bKeepSession);
				const TSharedRef<FJsonObject> Mesh = MakeShared<FJsonObject>();
				Mesh->SetStringField(TEXT("build_mode"), MeshBuild);
				Mesh->SetNumberField(TEXT("exposed_face_count"), FaceCount);
				Mesh->SetNumberField(TEXT("vertex_count"), NewVertexCount);
				Mesh->SetNumberField(TEXT("triangle_count"), NewTriangleCount);
				Mesh->SetStringField(TEXT("material_ref"), MaterialPath);
				Payload->SetObjectField(TEXT("mesh"), Mesh);
				const TSharedRef<FJsonObject> CollisionResult =
					MakeShared<FJsonObject>();
				CollisionResult->SetStringField(TEXT("mode"), CollisionMode);
				CollisionResult->SetNumberField(TEXT("box_count"), Boxes.Num());
				CollisionResult->SetNumberField(TEXT("convex_count"), NewSimpleCollision.ConvexElems.Num());
				if (!Session->bSolidGeometry) CollisionResult->SetNumberField(TEXT("covered_cell_count"), Session->Cells.Num());
				CollisionResult->SetBoolField(
					TEXT("exact_cover"), CollisionMode == TEXT("none")
						? Boxes.IsEmpty() : true);
				Payload->SetObjectField(TEXT("collision"), CollisionResult);
				const TSharedRef<FJsonObject> StateOwner = MakeShared<FJsonObject>();
				StateOwner->SetStringField(TEXT("kind"), TEXT("actor_component"));
				StateOwner->SetStringField(
					TEXT("schema"), GetAuthoringStateSchema());
				StateOwner->SetStringField(
					TEXT("component_path"), StateComponent->GetPathName());
				Payload->SetObjectField(TEXT("authoring_state"), StateOwner);
				const TSharedRef<FJsonObject> Durability = MakeShared<FJsonObject>();
				Durability->SetStringField(
					TEXT("receipt_id"), Session->DurableReceiptId);
				Durability->SetStringField(
					TEXT("atomicity"), TEXT("compensated_saga"));
				Durability->SetBoolField(
					TEXT("persisted_readback_verified"), true);
				Payload->SetObjectField(TEXT("durability"), Durability);
				Payload->SetObjectField(TEXT("readback"), PersistedReadback);
				Payload->SetArrayField(TEXT("errors"), {});
				Payload->SetArrayField(TEXT("warnings"), {});
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
					bool bActorReleased = true;
					if (Session.bCreatedActor && !Session.bCommitted)
					{
						if (ADynamicMeshActor* Actor = Session.Actor.Get())
						{
							bActorReleased = Actor->GetWorld()
								? Actor->GetWorld()->DestroyActor(Actor) : false;
						}
					}
					UE::UAI::Contract::FDiagnostic RollbackError;
					TryRollbackDurability(
						Session, bActorReleased, RollbackError);
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

			bool TryPrepareDurability(
				const FUeAgentCommandRequest& Request,
				UWorld& World,
				ADynamicMeshActor* Actor,
				const bool bCreate,
				FSession& Session,
				UE::UAI::Contract::FDiagnostic& OutError)
			{
				OutError.Reset();
				Session.DurableSession.Reset();
				Session.DurableResources = MakeDurableResources();
				Session.FencingToken = ++NextFencingToken;
				Session.DurableJournalOwnerId =
					TEXT("modeling-voxel-grid:") + Session.SessionId;
				const FPlanningPreflightPlanRef Plan =
					MakeDurablePlan(Request, Session, bCreate);
				Session.DurablePlanFingerprint = Plan->GetFingerprint();
				if (Session.DurablePlanFingerprint.IsEmpty())
				{
					OutError = ModelingCondition(TEXT("voxel_grid_durable_fingerprint_failed"),
						TEXT("The voxel durability plan did not produce a fingerprint."),
						TEXT("/durability/plan_fingerprint_available"), false, true,
						Actor ? Actor->GetPathName() : World.GetPathName());
					OutError.SubjectId = Session.SessionId;
					return false;
				}

				TArray<FString> PackageNames;
				if (UPackage* WorldPackage = World.GetOutermost())
				{
					PackageNames.Add(WorldPackage->GetName());
				}
				if (Actor && Actor->GetOutermost())
				{
					PackageNames.AddUnique(Actor->GetOutermost()->GetName());
				}
				if (PackageNames.IsEmpty())
				{
					OutError = ModelingFact(TEXT("voxel_grid_durable_package_missing"),
						TEXT("Neither the voxel world nor its actor provides a package to persist."),
						TEXT("/durability/package_count"), MakeShared<FJsonValueNumber>(0),
						MakeShared<FJsonValueString>(TEXT("at least one package")),
						Actor ? Actor->GetPathName() : World.GetPathName());
					return false;
				}
				for (const FString& PackageName : PackageNames)
				{
					if (!FPackageName::IsValidLongPackageName(PackageName))
					{
						OutError = ModelingFact(TEXT("voxel_grid_durable_package_invalid"),
							TEXT("The voxel durability target is not a valid Unreal long package name."),
							TEXT("/durability/package_name"), MakeShared<FJsonValueString>(PackageName),
							MakeShared<FJsonValueString>(TEXT("valid Unreal long package name")),
							Actor ? Actor->GetPathName() : World.GetPathName());
						return false;
					}
					if (UPackage* Package = FindPackage(nullptr, *PackageName))
					{
						if (Package->IsDirty())
						{
							OutError = ModelingCondition(TEXT("voxel_grid_target_package_dirty"),
								TEXT("The voxel target package has unsaved changes; save or discard them before starting the durability session."),
								TEXT("/durability/package_dirty"), true, false, PackageName);
							return false;
						}
					}
				}

				const FUeAgentCanonicalIdentity Identity = DurableIdentity(Session);
				TArray<FAssetDurableTarget> Targets;
				for (const FString& PackageName : PackageNames)
				{
					Targets.Emplace(
						Identity,
						PackageName,
						EPackageHeaderKind::Map,
						EAssetDurableIntent::Save,
						ERollbackGrade::DurableBackup);
				}
				FAssetDurableError DurableError;
				if (!TryPrepareAssetDurability(
						FAssetDurableRequest(
							Plan,
							MoveTemp(Targets),
							Session.FencingToken,
							Session.DurableJournalOwnerId,
							EAtomicityMode::CompensatedSaga),
						DurableBackend,
						FPaths::ProjectSavedDir()
							/ TEXT("UeAgentInterface/VoxelGrid/Durable"),
						*Session.DurableResources,
						Session.DurableSession,
						DurableError)
					|| !Session.DurableSession.IsValid())
				{
					OutError = DurableError;
					return false;
				}
				return true;
			}

			bool TryEnlistActorPackage(
				FSession& Session,
				ADynamicMeshActor& Actor,
				UE::UAI::Contract::FDiagnostic& OutError)
			{
				OutError.Reset();
				if (!Session.DurableSession.IsValid()
					|| !Session.DurableResources.IsValid()
					|| !Actor.GetOutermost())
				{
					const FString Field = !Session.DurableSession.IsValid() ? TEXT("/durability/session_available")
						: !Session.DurableResources.IsValid() ? TEXT("/durability/resources_available") : TEXT("/outer_package_available");
					const FString Reason = !Session.DurableSession.IsValid() ? TEXT("No durability session is available to enlist the voxel actor package.")
						: !Session.DurableResources.IsValid() ? TEXT("The voxel durability session has no resource tracker.")
						: TEXT("The voxel actor has no outer package to enlist.");
					OutError = ModelingCondition(TEXT("voxel_grid_durable_session_missing"), Reason, Field, false, true, Actor.GetPathName());
					OutError.SubjectId = Session.SessionId;
					return false;
				}
				const FString PackageName = Actor.GetOutermost()->GetName();
				const bool bAlreadyPresent = Session.DurableSession->GetRequest()
					.GetTargets().ContainsByPredicate(
						[&PackageName](const FAssetDurableTarget& Target)
						{
							return Target.GetPackageName() == PackageName;
						});
				if (bAlreadyPresent) return true;
				FAssetDurableSessionPtr Extended;
				FAssetDurableError DurableError;
				if (!TryEnlistAssetDurableTargets(
						Session.DurableSession.ToSharedRef(),
						TArray<FAssetDurableTarget>{
							FAssetDurableTarget(
								DurableIdentity(Session),
								PackageName,
								EPackageHeaderKind::Map,
								EAssetDurableIntent::Save,
								ERollbackGrade::DurableBackup)},
						DurableBackend,
						Session.FencingToken,
						*Session.DurableResources,
						Extended,
						DurableError)
					|| !Extended.IsValid())
				{
					OutError = DurableError;
					return false;
				}
				Session.DurableSession = MoveTemp(Extended);
				return true;
			}

			bool TryRollbackDurability(
				FSession& Session,
				const bool bMemoryRollbackConfirmed,
				UE::UAI::Contract::FDiagnostic& OutError)
			{
				OutError.Reset();
				if (!Session.DurableSession.IsValid())
				{
					if (!bMemoryRollbackConfirmed)
					{
						OutError = ModelingCondition(TEXT("voxel_grid_memory_rollback_unconfirmed"),
							TEXT("The voxel actor memory rollback could not be confirmed."),
							TEXT("/memory_rollback_confirmed"), false, true,
							Session.Actor.IsValid() ? Session.Actor->GetPathName() : FString());
						OutError.SubjectId = Session.SessionId;
					}
					return bMemoryRollbackConfirmed;
				}
				if (!Session.DurableResources.IsValid())
				{
					OutError = ModelingCondition(TEXT("voxel_grid_durable_resources_missing"),
						TEXT("The voxel durability resource tracker is unavailable for rollback."),
						TEXT("/durability/resources_available"), false, true,
						Session.Actor.IsValid() ? Session.Actor->GetPathName() : FString());
					OutError.SubjectId = Session.SessionId;
					return false;
				}
				FDurableReceipt Receipt;
				FAssetDurableError DurableError;
				const bool bRolledBack = TryRollbackAssetDurability(
					Session.DurableSession.ToSharedRef(),
					DurableBackend,
					Session.FencingToken,
					bMemoryRollbackConfirmed,
					*Session.DurableResources,
					Receipt,
					DurableError);
				if (!bRolledBack
					|| Receipt.GetStatus() != EDurableStatus::RolledBack)
				{
					OutError = DurableError;
					return false;
				}
				for (const FAssetDurableTarget& Target
					: Session.DurableSession->GetRequest().GetTargets())
				{
					if (UPackage* Package = FindPackage(
							nullptr, *Target.GetPackageName()))
					{
						Package->SetDirtyFlag(false);
					}
				}
				Session.DurableSession.Reset();
				Session.DurableResources.Reset();
				return true;
			}

			bool TryMoveLegacySidecarToTombstone(
				FSession& Session,
				FString& OutTombstonePath,
				UE::UAI::Contract::FDiagnostic& OutError)
			{
				OutTombstonePath.Reset();
				OutError.Reset();
				if (!Session.bLegacySidecarImported) return true;
				if (!IFileManager::Get().FileExists(*Session.LegacySidecarPath)
					|| Session.DurableReceiptId.IsEmpty())
				{
					const bool bSourceExists = IFileManager::Get().FileExists(*Session.LegacySidecarPath);
					OutError = ModelingCondition(TEXT("voxel_grid_legacy_sidecar_migration_source_missing"), bSourceExists
						? TEXT("The legacy voxel sidecar cannot be migrated because the durability receipt is missing.")
						: TEXT("The legacy voxel sidecar file no longer exists."), bSourceExists
						? TEXT("/durability/receipt_available") : TEXT("/legacy_sidecar_exists"), false, true,
						Session.Actor.IsValid() ? Session.Actor->GetPathName() : FString());
					OutError.Source.SourceFile = Session.LegacySidecarPath;
					OutError.ActualEvidence = MakeShared<FJsonValueString>(Session.DurableReceiptId);
					return false;
				}
				const FString TombstoneRoot = FPaths::ProjectSavedDir()
					/ TEXT("UeAgentInterface/VoxelGridMigrationTombstones")
					/ Session.DurableReceiptId;
				if (!IFileManager::Get().MakeDirectory(*TombstoneRoot, true)
					&& !IFileManager::Get().DirectoryExists(*TombstoneRoot))
				{
					OutError = ModelingCondition(TEXT("voxel_grid_legacy_sidecar_tombstone_directory_failed"),
						TEXT("The legacy voxel sidecar migration directory could not be created."),
						TEXT("/directory_available"), false, true);
					OutError.Source.SourceFile = TombstoneRoot;
					return false;
				}
				OutTombstonePath = TombstoneRoot
					/ FPaths::GetCleanFilename(Session.LegacySidecarPath);
				if (!IFileManager::Get().Move(
						*OutTombstonePath,
						*Session.LegacySidecarPath,
						false,
						true,
						false,
						true))
				{
					const TSharedRef<FJsonObject> Actual = MakeShared<FJsonObject>();
					Actual->SetStringField(TEXT("source_file"), Session.LegacySidecarPath);
					Actual->SetStringField(TEXT("destination_file"), OutTombstonePath);
					Actual->SetBoolField(TEXT("moved"), false);
					OutError = ModelingFact(TEXT("voxel_grid_legacy_sidecar_tombstone_move_failed"),
						TEXT("The legacy voxel sidecar could not be moved to the migration directory."),
						TEXT("/migration_move"), MakeShared<FJsonValueObject>(Actual),
						MakeShared<FJsonValueString>(TEXT("sidecar moved to destination")));
					OutError.Source.SourceFile = Session.LegacySidecarPath;
					OutTombstonePath.Reset();
					return false;
				}
				return true;
			}

			bool TryPersistAndVerify(
				FSession& Session,
				const FSession& Intended,
				const FString& MaterialPath,
				TSharedRef<FJsonObject>& OutPersistedReadback,
				UE::UAI::Contract::FDiagnostic& OutError)
			{
				OutPersistedReadback = MakeShared<FJsonObject>();
				OutError.Reset();
				const UDynamicMeshComponent* ExpectedComponent = DynamicMeshComponent(Intended.Actor.Get());
				const TArray<float> ExpectedPrimitiveData = ExpectedComponent ? ExpectedComponent->GetDefaultCustomPrimitiveData().Data : TArray<float>();
				if (!Session.DurableSession.IsValid()
					|| !Session.DurableResources.IsValid())
				{
					OutError = ModelingCondition(TEXT("voxel_grid_durable_session_missing"),
						!Session.DurableSession.IsValid() ? TEXT("No voxel durability session is available to persist and verify.")
						: TEXT("The voxel durability resource tracker is unavailable for persistence."),
						!Session.DurableSession.IsValid() ? TEXT("/durability/session_available") : TEXT("/durability/resources_available"),
						false, true, Session.Actor.IsValid() ? Session.Actor->GetPathName() : FString());
					OutError.SubjectId = Session.SessionId;
					return false;
				}
				const FAssetDurableSessionRef Prepared =
					Session.DurableSession.ToSharedRef();
				FDurableReceipt Receipt;
				FAssetDurableError DurableError;
				if (!TryPersistAssetDurabilityForVerification(
						Prepared,
						DurableBackend,
						Session.FencingToken,
						*Session.DurableResources,
						Receipt,
						DurableError))
				{
					OutError = DurableError;
					return false;
				}
				Session.DurableReceiptId = Receipt.GetJournalId();
				Session.DurableSession = MakeShared<const FAssetDurableSession>(
					Prepared->GetRequest(),
					Prepared->GetInventory(),
					Prepared->GetJournalId(),
					Prepared->GetJournalPath(),
					Prepared->GetJournalRoot(),
					Prepared->GetTransactionRoot(),
					Prepared->GetStagingRoot(),
					Receipt.GetPhaseHistory());

				bool bReadbackEquivalent = true;
				FPersistedWorldLoadError LoadError;
				const bool bObserved = TryObservePersistedWorld(
					Session.DurableSession->GetInventory().GetArtifacts(),
					[&](UWorld& PersistedWorld,
						const FPersistedWorldReferenceMap&,
						FPersistedWorldLoadError& ObserverError)
					{
						AActor* PersistedActor = nullptr;
						UE::UAI::Contract::FDiagnostic ResolveError;
						if (!UeAgentLevelAuthoring::TryFindManagedActorExact(
								PersistedWorld,
								Intended.GridId,
								UeAgentLevelAuthoring::MakeManagedActorFolderPath(
									TEXT("modeling-voxel-grid")),
								PersistedActor,
								ResolveError))
						{
							bReadbackEquivalent = false;
							UeAgentAuthoringDiagnostics::ReportConsistencyWarning(TEXT("voxel_grid_persisted_actor_not_found"),
								ResolveError, TEXT("voxel_grid_observation"));
							return true;
						}
						ADynamicMeshActor* PersistedDynamic =
							Cast<ADynamicMeshActor>(PersistedActor);
						UE::UAI::Contract::FDiagnostic VerifyError;
						if (!PersistedDynamic)
						{
							VerifyError = ModelingFact(TEXT("voxel_grid_persisted_actor_type_mismatch"), PersistedActor
								? TEXT("The persisted voxel actor is not a DynamicMeshActor.")
								: TEXT("The persisted voxel actor is unavailable for readback."), TEXT("/actor/class"),
								PersistedActor ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(PersistedActor->GetClass()->GetPathName()))
								: TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()),
								MakeShared<FJsonValueString>(ADynamicMeshActor::StaticClass()->GetPathName()),
								Intended.Actor.IsValid() ? Intended.Actor->GetPathName() : FString());
						}
						if (!PersistedDynamic
							|| !VerifyAppliedState(
								Intended,
								*PersistedDynamic,
								MaterialPath,
								ExpectedPrimitiveData,
								OutPersistedReadback,
								VerifyError))
						{
							bReadbackEquivalent = false;
							UeAgentAuthoringDiagnostics::ReportConsistencyWarning(TEXT("voxel_grid_persisted_readback_mismatch"),
								VerifyError, TEXT("voxel_grid_observation"));
						}
						return true;
					},
					LoadError);
				if (!bObserved)
				{
					OutError = LoadError;
					return false;
				}

				FString TombstonePath;
				if (!TryMoveLegacySidecarToTombstone(
						Session, TombstonePath, OutError))
				{
					return false;
				}
				const FDurableVerificationAcknowledgement Acknowledgement(
					Session.DurableReceiptId,
					Session.DurablePlanFingerprint,
					Session.DurableSession->GetInventory().GetInventoryHash(),
					TEXT("voxel-grid-persisted-readback:") + Session.GridId,
					Session.DurableJournalOwnerId,
					Session.FencingToken, bReadbackEquivalent);
				if (!TryAcknowledgeAssetDurability(
						Session.DurableSession.ToSharedRef(),
						Acknowledgement,
						Session.FencingToken,
						*Session.DurableResources,
						DurableError))
				{
					if (!TombstonePath.IsEmpty())
					{
						IFileManager::Get().Move(
							*Session.LegacySidecarPath,
							*TombstonePath,
							true,
							true,
							false,
							true);
					}
					OutError = DurableError;
					return false;
				}

				for (const FAssetDurableTarget& Target
					: Session.DurableSession->GetRequest().GetTargets())
				{
					if (UPackage* Package = FindPackage(
							nullptr, *Target.GetPackageName()))
					{
						Package->SetDirtyFlag(false);
					}
				}
				OutPersistedReadback->SetStringField(
					TEXT("receipt_id"), Session.DurableReceiptId);
				OutPersistedReadback->SetBoolField(
					TEXT("persisted_readback_verified"), bReadbackEquivalent);
				OutPersistedReadback->SetNumberField(
					TEXT("artifact_count"),
					Session.DurableSession->GetInventory().GetArtifacts().Num());
				Session.DurableSession.Reset();
				Session.DurableResources.Reset();
				Session.bLegacySidecarImported = false;
				return true;
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
					SetFailure(OutFailure, ModelingInputFailure(TEXT("voxel_grid_begin_mode_unsupported"),
						TEXT("Voxel begin mode must be create or edit; modify is accepted as an alias for edit."),
						Params, TEXT("mode"), MakeShared<FJsonValueString>(TEXT("create, edit, or modify"))),
						TEXT("not_started"), true, false, UE::UAI::Contract::FDiagnostic());
					return false;
				}
				FString SessionId;
				Params->TryGetStringField(TEXT("session_id"), SessionId);
				if (SessionId.IsEmpty()) SessionId = MakeGuid(TEXT("VGS_"));
				if (!IsSafeIdentity(SessionId) || Sessions.Contains(SessionId))
				{
					const bool bSafeIdentity = IsSafeIdentity(SessionId);
					FFailure Conflict = ModelingCondition(TEXT("voxel_grid_session_id_conflict"), bSafeIdentity
						? TEXT("The managed voxel session reference is already in use.")
						: TEXT("The managed voxel session reference contains unsupported characters."),
						bSafeIdentity ? TEXT("/session_reference_available") : TEXT("/session_reference_valid"), false, true);
					Conflict.ActualEvidence = MakeShared<FJsonValueString>(SessionId);
					if (Params->HasField(TEXT("session_id")))
					{
						Conflict.SourceKind = TEXT("command_input"); Conflict.Source.JsonPath = TEXT("/session_id");
						Conflict.SemanticPointer = Conflict.Source.JsonPath;
					}
					SetFailure(OutFailure, Conflict, TEXT("not_started"), true, false, UE::UAI::Contract::FDiagnostic());
					return false;
				}
				for (auto It = Sessions.CreateIterator(); It; ++It)
				{
					if (!It.Value().Actor.IsValid()) It.RemoveCurrent();
				}

				FSession Session;
				Session.SessionId = SessionId;
				const bool bGridIdFromInput = Params->TryGetStringField(TEXT("grid_id"), Session.GridId) && !Session.GridId.IsEmpty();
				FString ActorRef;
				Params->TryGetStringField(TEXT("actor_ref"), ActorRef);
				ADynamicMeshActor* Actor = nullptr;
				UE::UAI::Contract::FDiagnostic Error;
				if (Mode == TEXT("create"))
				{
					if (Session.GridId.IsEmpty()) Session.GridId = MakeGuid(TEXT("VG_"));
					const bool bSafeGridIdentity = IsSafeIdentity(Session.GridId);
					const bool bSidecarExists = bSafeGridIdentity && IFileManager::Get().FileExists(*LegacySidecarPath(Session.GridId));
					const bool bActorExists = bSafeGridIdentity && !bSidecarExists && HasActorForGrid(World, Session.GridId);
					if (!bSafeGridIdentity || bSidecarExists || bActorExists)
					{
						Error = ModelingCondition(TEXT("voxel_grid_grid_id_conflict"), !bSafeGridIdentity
							? TEXT("The managed voxel grid identity contains unsupported characters.")
							: bSidecarExists ? TEXT("A legacy voxel sidecar already exists for this managed grid.")
							: TEXT("An actor already owns this managed voxel grid identity."),
							!bSafeGridIdentity ? TEXT("/grid_identity_valid")
							: bSidecarExists ? TEXT("/legacy_sidecar_exists") : TEXT("/actor_exists"),
							bSafeGridIdentity, !bSafeGridIdentity, World.GetPathName());
						Error.ActualEvidence = MakeShared<FJsonValueString>(Session.GridId);
						if (bSidecarExists) Error.Source.SourceFile = LegacySidecarPath(Session.GridId);
						else if (!bSafeGridIdentity && bGridIdFromInput)
						{
							Error.SourceKind = TEXT("command_input"); Error.Source.JsonPath = TEXT("/grid_id");
							Error.SemanticPointer = Error.Source.JsonPath;
						}
						SetFailure(OutFailure, Error, TEXT("not_started"), true, false, UE::UAI::Contract::FDiagnostic());
						return false;
					}
					double CellSize = 100.0;
					Params->TryGetNumberField(TEXT("cell_size"), CellSize);
					if (!FMath::IsFinite(CellSize)
						|| CellSize <= KINDA_SMALL_NUMBER)
					{
						Error = ModelingInputFailure(TEXT("voxel_grid_cell_size_invalid"),
							TEXT("Voxel cell size must be finite and greater than the minimum positive tolerance."),
							Params, TEXT("cell_size"), MakeShared<FJsonValueString>(FString::Printf(TEXT("finite number greater than %.17g"), static_cast<double>(KINDA_SMALL_NUMBER))));
						SetFailure(OutFailure, Error, TEXT("not_started"), true, false, UE::UAI::Contract::FDiagnostic());
						return false;
					}
					FVector Origin = FVector::ZeroVector;
					FRotator Rotation = FRotator::ZeroRotator;
					FVector Anchor = FVector::ZeroVector;
					if (Params->HasField(TEXT("origin"))
						&& !TryReadVector(Params, TEXT("origin"), Origin, &Error))
					{
						SetFailure(OutFailure, FFailure::WrapFailure(TEXT("voxel_grid_origin_invalid"), Error, TEXT("voxel_grid"),
							TEXT("Voxel origin contains an invalid coordinate.")), TEXT("not_started"), true, false, UE::UAI::Contract::FDiagnostic());
						return false;
					}
					if (Params->HasField(TEXT("rotation"))
						&& !TryReadRotator(Params, TEXT("rotation"), Rotation, &Error))
					{
						SetFailure(OutFailure, FFailure::WrapFailure(TEXT("voxel_grid_rotation_invalid"), Error, TEXT("voxel_grid"),
							TEXT("Voxel rotation contains an invalid angle.")), TEXT("not_started"), true, false, UE::UAI::Contract::FDiagnostic());
						return false;
					}
					if (Params->HasField(TEXT("anchor"))
						&& !TryReadVector(Params, TEXT("anchor"), Anchor, &Error))
					{
						SetFailure(OutFailure, FFailure::WrapFailure(TEXT("voxel_grid_anchor_invalid"), Error, TEXT("voxel_grid"),
							TEXT("Voxel anchor contains an invalid coordinate.")), TEXT("not_started"), true, false, UE::UAI::Contract::FDiagnostic());
						return false;
					}
					Session.CellSize = CellSize;
					Session.GridToWorld = FTransform(
						Rotation, Origin, FVector::OneVector);
					Session.AnchorLocal = Anchor;
					if (!TryPrepareDurability(
							Request, World, nullptr, true, Session, Error))
					{
						SetFailure(OutFailure, Error, TEXT("not_started"), true, false, UE::UAI::Contract::FDiagnostic());
						return false;
					}
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
						UE::UAI::Contract::FDiagnostic RollbackError;
						const bool bRolledBack = TryRollbackDurability(
							Session, true, RollbackError);
						const TSharedRef<FJsonObject> Actual = MakeShared<FJsonObject>();
						Actual->SetBoolField(TEXT("spawned"), false);
						Actual->SetStringField(TEXT("transform"), InitialTransform.ToHumanReadableString());
						FFailure SpawnError = ModelingFact(TEXT("voxel_grid_actor_spawn_failed"),
							TEXT("Unreal could not spawn a DynamicMeshActor for the voxel grid."),
							TEXT("/actor_spawn"), MakeShared<FJsonValueObject>(Actual),
							MakeShared<FJsonValueString>(TEXT("spawned DynamicMeshActor")), World.GetPathName());
						SpawnError.SubjectId = Session.SessionId;
						SetFailure(OutFailure, SpawnError, bRolledBack ? FString(TEXT("rolled_back"))
								: FString(TEXT("state_unknown")), bRolledBack, true, RollbackError);
						return false;
					}
					Session.Actor = Actor;
					Session.bCreatedActor = true;
					Session.bDirty = true;
					Session.bCommitted = false;
					if (!TryEnlistActorPackage(Session, *Actor, Error))
					{
						const auto EnlistError = Error;
						const bool bActorReleased = World.DestroyActor(Actor);
						UE::UAI::Contract::FDiagnostic RollbackError;
						const bool bRolledBack = TryRollbackDurability(
							Session, bActorReleased, RollbackError);
						SetFailure(OutFailure, EnlistError, bRolledBack ? FString(TEXT("rolled_back"))
								: FString(TEXT("state_unknown")), bRolledBack, true, RollbackError);
						return false;
					}
					FString Label;
					if (Params->TryGetStringField(TEXT("label"), Label)
						&& !Label.IsEmpty()) Actor->SetActorLabel(Label);
					Actor->Tags.AddUnique(FName(*ActorTag(Session.GridId)));
					if (!UeAgentLevelAuthoring::TryBindManagedActorOwnership(
							*Actor,
							Session.GridId,
							TEXT("modeling-voxel-grid"),
							Error))
					{
						const auto OwnershipError = Error;
						const bool bActorReleased = World.DestroyActor(Actor);
						UE::UAI::Contract::FDiagnostic RollbackError;
						const bool bRolledBack = TryRollbackDurability(
							Session, bActorReleased, RollbackError);
						SetFailure(OutFailure, OwnershipError, bRolledBack ? FString(TEXT("rolled_back"))
								: FString(TEXT("state_unknown")), bRolledBack, true, RollbackError);
						return false;
					}
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
						SetFailure(OutFailure, Error, TEXT("not_started"), true, false, UE::UAI::Contract::FDiagnostic());
						return false;
					}
					if (Session.GridId.IsEmpty()
						&& !TryGridIdFromActor(*Actor, Session.GridId))
					{
						Error = ModelingCondition(TEXT("voxel_grid_grid_id_required"),
							TEXT("The selected actor has no managed voxel grid identity and none was supplied."),
							TEXT("/managed_grid_identity_available"), false, true, Actor->GetPathName());
						SetFailure(OutFailure, Error, TEXT("not_started"), true, false, UE::UAI::Contract::FDiagnostic());
						return false;
					}
					if (!IsSafeIdentity(Session.GridId))
					{
						Error = ModelingCondition(TEXT("voxel_grid_grid_id_invalid"),
							TEXT("The managed voxel grid identity contains unsupported characters."),
							TEXT("/managed_grid_identity_valid"), false, true, Actor->GetPathName());
						Error.ActualEvidence = MakeShared<FJsonValueString>(Session.GridId);
						if (bGridIdFromInput)
						{
							Error.SourceKind = TEXT("command_input"); Error.Source.JsonPath = TEXT("/grid_id");
							Error.SemanticPointer = Error.Source.JsonPath;
						}
						SetFailure(OutFailure, Error, TEXT("not_started"), true, false, UE::UAI::Contract::FDiagnostic());
						return false;
					}
					FString TaggedGridId;
					if (!TryGridIdFromActor(*Actor, TaggedGridId)
						|| TaggedGridId != Session.GridId)
					{
						Error = ModelingCondition(TEXT("voxel_grid_actor_grid_identity_mismatch"),
							TEXT("The selected actor does not carry the requested managed voxel grid identity."),
							TEXT("/managed_grid_identity_matches"), false, true, Actor->GetPathName());
						Error.ActualEvidence = MakeShared<FJsonValueString>(TaggedGridId);
						Error.ExpectedEvidence = MakeShared<FJsonValueString>(Session.GridId);
						SetFailure(OutFailure, Error, TEXT("not_started"), true, false, UE::UAI::Contract::FDiagnostic());
						return false;
					}
					Session.Actor = Actor;
					Session.LegacySidecarPath = LegacySidecarPath(Session.GridId);
					UUeAgentModelingVoxelGridStateComponent* StateComponent = nullptr;
					UE::UAI::Contract::FDiagnostic StateError;
					if (TryFindAuthoringStateComponent(
							*Actor, StateComponent, StateError))
					{
						if (IFileManager::Get().FileExists(
								*Session.LegacySidecarPath))
						{
							Error = ModelingCondition(TEXT("voxel_grid_authoring_state_conflict"),
								TEXT("Both the actor authoring-state component and a legacy voxel sidecar exist; the authoring source is ambiguous."),
								TEXT("/legacy_sidecar_exists"), true, false, Actor->GetPathName());
							Error.Source.SourceFile = Session.LegacySidecarPath;
							SetFailure(OutFailure, Error, TEXT("not_started"), true, false, UE::UAI::Contract::FDiagnostic());
							return false;
						}
						FVoxelGridAuthoringState State;
						if (!TryReadAuthoringState(
								*StateComponent,
								State,
								StateError))
						{
							SetFailure(OutFailure, StateError, TEXT("not_started"), true, false, UE::UAI::Contract::FDiagnostic());
							return false;
						}
						if (State.GridId != Session.GridId)
						{
							StateError = ModelingCondition(TEXT("voxel_grid_authoring_state_identity_mismatch"),
								TEXT("The actor's serialized voxel state belongs to a different managed grid."),
								TEXT("/authoring_state/grid_identity_matches"), false, true, Actor->GetPathName());
							StateError.ActualEvidence = MakeShared<FJsonValueString>(State.GridId);
							StateError.ExpectedEvidence = MakeShared<FJsonValueString>(Session.GridId);
							SetFailure(OutFailure, StateError, TEXT("not_started"), true, false, UE::UAI::Contract::FDiagnostic());
							return false;
						}
						ApplyAuthoringStateToSession(State, Session);
					}
					else if (StateError.Code == TEXT("voxel_grid_authoring_state_missing")
						&& IFileManager::Get().FileExists(
							*Session.LegacySidecarPath))
					{
						if (!LoadLegacySidecar(Session, Error))
						{
							SetFailure(OutFailure, Error, TEXT("not_started"), true, false, UE::UAI::Contract::FDiagnostic());
							return false;
						}
					}
					else
					{
						SetFailure(OutFailure, StateError, TEXT("not_started"), true, false, UE::UAI::Contract::FDiagnostic());
						return false;
					}
					if (!TryPrepareDurability(
							Request, World, Actor, false, Session, Error))
					{
						SetFailure(OutFailure, Error, TEXT("not_started"), true, false, UE::UAI::Contract::FDiagnostic());
						return false;
					}
				}
				Session.LegacySidecarPath = LegacySidecarPath(Session.GridId);
				Sessions.Add(SessionId, MoveTemp(Session));
				FSession& Stored = Sessions.FindChecked(SessionId);
				OutPayload = SuccessPayload(
					Request, TEXT("begin"), SessionObservation(Stored, Params));
				return true;
			}

			static void PromoteToSolid(FSession& Session)
			{
				if (Session.bSolidGeometry) return;
				for (const FVoxelGridCollisionBox& Box : BuildExactCoverBoxes(Session.Cells))
					Session.SolidPieces.Add(GridSolid::MakeBox(FVector(Box.Min) * Session.CellSize, FVector(Box.MaxExclusive) * Session.CellSize));
				Session.Cells.Reset();
				Session.bSolidGeometry = true;
			}

			bool MutateShape(const FUeAgentCommandRequest& Request, const FString& Action, FSession& Session,
				TSharedPtr<FJsonObject>& OutPayload, FFailure& OutFailure)
			{
				FFailure Error;
				const auto Params = Request.GetParams();
				if (!CheckRevision(Params, Session, Error)) { SetFailure(OutFailure, Error, TEXT("not_started"), true, false, {}); return false; }
				const TSharedPtr<FJsonObject>* ShapeObject = nullptr;
				if (!Params->TryGetObjectField(TEXT("shape"), ShapeObject) || !ShapeObject || !ShapeObject->IsValid())
				{
					Error = ModelingInputFailure(TEXT("voxel_grid_shape_required"), TEXT("A shape object with kind, space, center and size is required."),
						Params, TEXT("shape"), MakeShared<FJsonValueString>(TEXT("shape object")));
					SetFailure(OutFailure, Error, TEXT("not_started"), true, false, {}); return false;
				}
				const auto Shape = *ShapeObject;
				FString Kind, Space;
				FVector Center, Size;
				FRotator Rotation = FRotator::ZeroRotator;
				if (!Shape->TryGetStringField(TEXT("kind"), Kind)
					|| (Kind != TEXT("square_pyramid") && Kind != TEXT("triangular_prism") && Kind != TEXT("box")))
					Error = ModelingInputFailure(TEXT("voxel_grid_shape_kind_invalid"), TEXT("Choose a supported exact solid shape."), Shape,
						TEXT("kind"), MakeShared<FJsonValueString>(TEXT("square_pyramid, triangular_prism, or box")));
				else if (!Shape->TryGetStringField(TEXT("space"), Space)
					|| (Space != TEXT("grid_local") && Space != TEXT("component_local") && Space != TEXT("world")))
					Error = ModelingInputFailure(TEXT("voxel_grid_shape_space_invalid"), TEXT("Shape center and rotation require an explicit coordinate space."),
						Shape, TEXT("space"), MakeShared<FJsonValueString>(TEXT("grid_local, component_local, or world")));
				else if (TryReadVector(Shape, TEXT("center"), Center, &Error) && TryReadVector(Shape, TEXT("size"), Size, &Error))
				{
					const TCHAR* Axes[] = {TEXT("x"), TEXT("y"), TEXT("z")};
					for (int32 Axis = 0; Axis < 3 && Error.IsEmpty(); ++Axis)
						if (Size[Axis] <= 2 * GridSolid::Tolerance)
						{
							Error = ModelingFact(TEXT("voxel_grid_shape_size_invalid"), TEXT("Each shape dimension must be positive and larger than the geometry tolerance."),
								FString(TEXT("/size/")) + Axes[Axis], GridNumberFact(Size[Axis]), GridNumberFact(2 * GridSolid::Tolerance));
						}
					if (Error.IsEmpty() && Shape->HasField(TEXT("rotation"))) TryReadRotator(Shape, TEXT("rotation"), Rotation, &Error);
				}
				if (!Error.IsEmpty())
				{
					UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error, TEXT("/shape"), false, true);
					SetFailure(OutFailure, Error, TEXT("not_started"), true, false, {}); return false;
				}
				auto Operand = GridSolid::MakeShape(Kind, Size, FTransform(Rotation, Center));
				for (auto& Face : Operand.Faces) for (FVector& P : Face.Vertices)
				{
					if (Space == TEXT("world")) P = Session.GridToWorld.InverseTransformPosition(P);
					else if (Space == TEXT("component_local")) P += Session.AnchorLocal;
				}
				if (!GridSolid::Validate({Operand}, Error)) { SetFailure(OutFailure, Error, TEXT("not_started"), true, false, {}); return false; }
				FSession Proposed = Session;
				PromoteToSolid(Proposed);
				const double Before = GridSolid::Volume(Proposed.SolidPieces);
				if (Action == TEXT("add_shape")) GridSolid::Add(Proposed.SolidPieces, Operand);
				else GridSolid::Remove(Proposed.SolidPieces, Operand);
				if (!GridSolid::Validate(Proposed.SolidPieces, Error)) { SetFailure(OutFailure, Error, TEXT("not_started"), true, false, {}); return false; }
				++Proposed.Revision; Proposed.bDirty = true;
				Session = MoveTemp(Proposed);
				const auto Readback = SessionObservation(Session, Params);
				Readback->SetNumberField(TEXT("changed_volume_cm3"), FMath::Abs(GridSolid::Volume(Session.SolidPieces) - Before));
				Readback->SetObjectField(TEXT("shape"), Shape);
				OutPayload = SuccessPayload(Request, Action, Readback);
				return true;
			}

			bool MutateBounds(
				const FUeAgentCommandRequest& Request,
				const FString& Action,
				FSession& Session,
				TSharedPtr<FJsonObject>& OutPayload,
				FFailure& OutFailure)
			{
				UE::UAI::Contract::FDiagnostic Error;
				if (!CheckRevision(Request.GetParams(), Session, Error))
				{
					SetFailure(OutFailure, Error, TEXT("not_started"), true, false, UE::UAI::Contract::FDiagnostic());
					return false;
				}
				FVoxelGridCellRange Range;
				TSharedPtr<FJsonObject> BoundsReadback;
				if (!ParseBounds(
					Request.GetParams(), Session, Range, BoundsReadback, Error))
				{
					SetFailure(OutFailure, Error, TEXT("not_started"), true, false, UE::UAI::Contract::FDiagnostic());
					return false;
				}

				int32 Changed = 0;
				if (Session.bSolidGeometry)
				{
					auto Pieces = Session.SolidPieces;
					const double Before = GridSolid::Volume(Pieces);
					const auto Shape = GridSolid::MakeBox(FVector(Range.Min) * Session.CellSize, FVector(Range.MaxExclusive) * Session.CellSize);
					if (Action == TEXT("add_bounds")) GridSolid::Add(Pieces, Shape); else GridSolid::Remove(Pieces, Shape);
					if (!GridSolid::Validate(Pieces, Error)) { SetFailure(OutFailure, Error, TEXT("not_started"), true, false, {}); return false; }
					Session.SolidPieces = MoveTemp(Pieces); ++Session.Revision; Session.bDirty = true;
					const auto Readback = SessionObservation(Session, Request.GetParams());
					Readback->SetNumberField(TEXT("changed_volume_cm3"), FMath::Abs(GridSolid::Volume(Session.SolidPieces) - Before));
					Readback->SetObjectField(TEXT("bounds"), BoundsReadback);
					OutPayload = SuccessPayload(Request, Action, Readback); return true;
				}
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
				UE::UAI::Contract::FDiagnostic Error;
				if (!CheckRevision(Request.GetParams(), Session, Error))
				{
					SetFailure(OutFailure, Error, TEXT("not_started"), true, false, UE::UAI::Contract::FDiagnostic());
					return false;
				}
				if (Action == TEXT("close") && Session.bDirty)
				{
					SetFailure(OutFailure, ModelingCondition(TEXT("voxel_grid_session_dirty"),
						TEXT("The voxel session has unapplied changes; apply or cancel it before close."),
						TEXT("/session_dirty"), true, false, Session.Actor.IsValid() ? Session.Actor->GetPathName() : FString()),
						TEXT("not_started"), true, false, UE::UAI::Contract::FDiagnostic());
					return false;
				}
				const FString SessionId = Session.SessionId;
				const FString ActorTarget = Session.Actor.IsValid() ? Session.Actor->GetPathName() : FString();
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
				UE::UAI::Contract::FDiagnostic RollbackError;
				const bool bDurabilityReleased = TryRollbackDurability(
					Session, bActorReleased, RollbackError);
				const TSharedRef<FJsonObject> Readback = MakeShared<FJsonObject>();
				Readback->SetStringField(TEXT("session_id"), SessionId);
				Readback->SetBoolField(TEXT("released"), true);
				Readback->SetBoolField(TEXT("actor_destroyed"), bDestroyActor);
				Readback->SetBoolField(TEXT("actor_release_confirmed"), bActorReleased);
				Readback->SetBoolField(
					TEXT("durability_release_confirmed"), bDurabilityReleased);
				if (!bActorReleased || !bDurabilityReleased)
				{
					Session.bStateUnknown = true;
					if (bActorReleased) Sessions.Remove(SessionId);
					if (RollbackError.IsEmpty())
					{
						const TSharedRef<FJsonObject> Actual = MakeShared<FJsonObject>();
						Actual->SetBoolField(TEXT("actor_release_confirmed"), bActorReleased);
						Actual->SetBoolField(TEXT("durability_release_confirmed"), bDurabilityReleased);
						const TSharedRef<FJsonObject> Expected = MakeShared<FJsonObject>();
						Expected->SetBoolField(TEXT("actor_release_confirmed"), true);
						Expected->SetBoolField(TEXT("durability_release_confirmed"), true);
						RollbackError = ModelingFact(TEXT("voxel_grid_cancel_cleanup_unconfirmed"),
							TEXT("Voxel session cleanup could not be confirmed."), TEXT("/cleanup"),
							MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueObject>(Expected), ActorTarget);
						RollbackError.SubjectId = SessionId;
					}
					SetFailure(OutFailure, RollbackError, TEXT("state_unknown"), false, true, UE::UAI::Contract::FDiagnostic());
					return false;
				}
				Sessions.Remove(SessionId);
				OutPayload = SuccessPayload(Request, Action, Readback);
				return true;
			}

			FCriticalSection Mutex;
			FPackageAssetDurableBackend DurableBackend{FPaths::ProjectContentDir()};
			int64 NextFencingToken = 2000;
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
					SetFailure(Failure, ModelingFact(TEXT("voxel_grid_command_identity_mismatch"),
						TEXT("The voxel handler received a different command from the one it serves."),
						TEXT("/command"), MakeShared<FJsonValueString>(Request.GetCommandId()), MakeShared<FJsonValueString>(CommandId)),
						TEXT("not_started"), true, false, UE::UAI::Contract::FDiagnostic());
					return FUeAgentCommandResult::Failure(
						Request, TEXT("{}"), ErrorDiagnostic(CommandId, Failure));
				}
				if (!Caller.IsValid()
					|| !Caller.GetAuthorityScopes().Contains(
						TEXT("uai:modeling-geometry:write")))
				{
					const TSharedRef<FJsonObject> Actual = MakeShared<FJsonObject>();
					Actual->SetBoolField(TEXT("caller_valid"), Caller.IsValid());
					Actual->SetBoolField(TEXT("scope_granted"), Caller.IsValid()
						&& Caller.GetAuthorityScopes().Contains(TEXT("uai:modeling-geometry:write")));
					SetFailure(Failure, ModelingFact(TEXT("modeling_geometry_authority_required"),
						TEXT("A valid caller with uai:modeling-geometry:write authority is required."),
						TEXT("/authority"), MakeShared<FJsonValueObject>(Actual),
						MakeShared<FJsonValueString>(TEXT("valid caller with uai:modeling-geometry:write scope"))),
						TEXT("not_started"), true, false, UE::UAI::Contract::FDiagnostic());
					return FUeAgentCommandResult::Failure(
						Request, TEXT("{}"), ErrorDiagnostic(CommandId, Failure));
				}
				if (Cancellation->IsCancelled())
				{
					SetFailure(Failure, ModelingCondition(TEXT("operation_cancelled"),
						TEXT("The voxel operation was cancelled before execution."), TEXT("/cancelled"), true, false),
						TEXT("not_started"), true, false, UE::UAI::Contract::FDiagnostic());
					return FUeAgentCommandResult::Failure(
						Request, TEXT("{}"), ErrorDiagnostic(CommandId, Failure));
				}
				TSharedPtr<FJsonObject> Payload;
				const bool bOk = CommandId == GetSessionCommandId()
					? ProcessRegistry().ExecuteSession(Request, Payload, Failure)
					: CommandId == GetBeginCommandId()
						? ProcessRegistry().ExecuteSession(
							Request, Payload, Failure, true)
					: ProcessRegistry().ExecuteApply(Request, Payload, Failure);
				if (!bOk)
					return FUeAgentCommandResult::Failure(Request, TEXT("{}"), ErrorDiagnostic(CommandId, Failure));
				FString PayloadJson;
				FFailure EncodeError;
				if (!CommandContract::Json::TryCanonicalize(Payload, PayloadJson, EncodeError))
				{
					if (EncodeError.SourceKind.IsEmpty()) EncodeError.SourceKind = TEXT("native_result");
					EncodeError.TerminalStatus = TEXT("failed");
					EncodeError.MutationMayHaveStarted = true;
					EncodeError.bRetrySafe = false;
					return FUeAgentCommandResult::Failure(Request, TEXT("{}"), MoveTemp(EncodeError), TEXT("voxel_grid_observation_encode_failed"));
				}
				return FUeAgentCommandResult::Success(Request, PayloadJson);
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

	const FString& GetBeginCommandId()
	{
		static const FString Id(TEXT("mesh.voxel_grid.begin"));
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
			GetBeginCommandId(), GetSessionCommandId(), GetApplyCommandId(),
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutRange = {};
		OutError.Reset();
		if (!Bounds.IsValid
			|| !IsFiniteVector(Bounds.Min)
			|| !IsFiniteVector(Bounds.Max)
			|| !FMath::IsFinite(CellSize)
			|| CellSize <= KINDA_SMALL_NUMBER)
		{
			if (!Bounds.IsValid)
				OutError = ModelingCondition(TEXT("voxel_grid_bounds_invalid"), TEXT("The voxel bounds container is invalid."), TEXT("/bounds_valid"), false, true);
			else if (!IsFiniteVector(Bounds.Min) || !IsFiniteVector(Bounds.Max))
				OutError = ModelingFact(TEXT("voxel_grid_bounds_invalid"), TEXT("A voxel bounds endpoint contains a non-finite coordinate."), !IsFiniteVector(Bounds.Min) ? TEXT("/bounds/min") : TEXT("/bounds/max"), MakeShared<FJsonValueObject>(GridVectorFact(!IsFiniteVector(Bounds.Min) ? Bounds.Min : Bounds.Max)), MakeShared<FJsonValueString>(TEXT("finite vector")));
			else
				OutError = ModelingFact(TEXT("voxel_grid_bounds_invalid"), TEXT("The managed voxel cell size must be finite and greater than the engine's small-number tolerance."), TEXT("/cell_size"), GridNumberFact(CellSize), MakeShared<FJsonValueString>(FString::Printf(TEXT("finite number > %.17g"), static_cast<double>(KINDA_SMALL_NUMBER))));
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
			OutError = ModelingFact(TEXT("voxel_grid_bounds_space_unsupported"), TEXT("The voxel bounds coordinate space is unsupported."), TEXT("/bounds/space"), MakeShared<FJsonValueString>(InputSpace), MakeShared<FJsonValueString>(TEXT("world, grid_local or component_local")));
			OutError.SourceKind = TEXT("related_input"); OutError.Source.JsonPath = TEXT("/bounds/space");
			return false;
		}
		const FVector ScaledMin = GridBounds.Min / CellSize;
		const FVector ScaledMax = GridBounds.Max / CellSize;
		const double Minimum = static_cast<double>(MIN_int32) + 1.0;
		const double Maximum = static_cast<double>(MAX_int32) - 1.0;
		if (ScaledMin.GetMin() < Minimum || ScaledMin.GetMax() > Maximum
			|| ScaledMax.GetMin() < Minimum || ScaledMax.GetMax() > Maximum)
		{
			const auto Actual = MakeShared<FJsonObject>();
			Actual->SetObjectField(TEXT("scaled_min"), GridVectorFact(ScaledMin));
			Actual->SetObjectField(TEXT("scaled_max"), GridVectorFact(ScaledMax));
			Actual->SetField(TEXT("cell_size"), GridNumberFact(CellSize)); Actual->SetStringField(TEXT("input_space"), InputSpace);
			const auto Expected = MakeShared<FJsonObject>();
			Expected->SetNumberField(TEXT("minimum_coordinate"), Minimum); Expected->SetNumberField(TEXT("maximum_coordinate"), Maximum);
			OutError = ModelingFact(TEXT("voxel_grid_bounds_coordinate_overflow"), TEXT("The converted voxel cell coordinates lie outside the supported signed 32-bit interior range."), TEXT("/scaled_cell_bounds"), MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueObject>(Expected));
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
			const auto Actual = MakeShared<FJsonObject>();
			Actual->SetObjectField(TEXT("min"), IntVectorJson(OutRange.Min)); Actual->SetObjectField(TEXT("max_exclusive"), IntVectorJson(OutRange.MaxExclusive));
			OutError = ModelingFact(TEXT("voxel_grid_bounds_empty"), TEXT("The calculated voxel cell range is empty on at least one axis."), TEXT("/cell_range"), MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(TEXT("max_exclusive greater than min on every axis")));
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutHandler.Reset();
		OutError.Reset();
		if (!IsCommand(CommandId))
		{
			OutError = ModelingFact(TEXT("voxel_grid_mechanics_command_unknown"),
				TEXT("No voxel mechanics handler serves the requested command."), TEXT("/command"),
				MakeShared<FJsonValueString>(CommandId), MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
					MakeShared<FJsonValueString>(GetSessionCommandId()), MakeShared<FJsonValueString>(GetBeginCommandId()),
					MakeShared<FJsonValueString>(GetApplyCommandId())}));
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

	void SetAutomationWorldOverride(UWorld* World)
	{
		GAutomationWorldOverride = World;
	}
#endif
}

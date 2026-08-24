#include "DomainAdapters/ModelingGeometryEffect/UeAgentModelingGeometryEffectCapabilities.h"
#include "DomainAdapters/ModelingGeometryEffect/UeAgentModelingGeometryEffectBoundarySemantics.h"

#include "Components/DynamicMeshComponent.h"
#include "CleaningOps/HoleFillOp.h"
#include "CleaningOps/RemeshMeshOp.h"
#include "CompositionOps/VoxelBooleanMeshesOp.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAABBTree3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMesh/DynamicMeshOverlay.h"
#include "DynamicMesh/MeshNormals.h"
#include "DynamicMesh/MeshTransforms.h"
#include "DynamicMeshActor.h"
#include "DynamicMeshToMeshDescription.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GeometryScript/CollisionFunctions.h"
#include "GeometryScript/MeshBasicEditFunctions.h"
#include "GeometryScript/MeshBooleanFunctions.h"
#include "GeometryScript/MeshModelingFunctions.h"
#include "GeometryScript/MeshNormalsFunctions.h"
#include "GeometryScript/MeshPrimitiveFunctions.h"
#include "GeometryScript/MeshRemeshFunctions.h"
#include "GeometryScript/MeshRepairFunctions.h"
#include "GeometryScript/MeshSelectionFunctions.h"
#include "GeometryScript/MeshSimplifyFunctions.h"
#include "GeometryScript/MeshSubdivideFunctions.h"
#include "GeometryScript/MeshTransformFunctions.h"
#include "GeometryScript/MeshUVFunctions.h"
#include "GeometryScript/MeshVoxelFunctions.h"
#include "Materials/MaterialInterface.h"
#include "Math/RotationMatrix.h"
#include "MeshBoundaryLoops.h"
#include "MeshDescriptionToDynamicMesh.h"
#include "MeshConstraintsUtil.h"
#include "MeshSimplification.h"
#include "Misc/ScopeLock.h"
#include "Operations/MeshPlaneCut.h"
#include "Operations/OffsetMeshRegion.h"
#include "PhysicsEngine/AggregateGeom.h"
#include "PhysicsEngine/BodySetup.h"
#include "ProjectionTargets.h"
#include "StaticMeshAttributes.h"
#include "TransformTypes.h"
#include "UDynamicMesh.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace UE::UAI::DomainAdapters::ModelingGeometryEffect
{
	using namespace ContextResource;
	using namespace ExecutionMutation;
	using namespace GenericEffect;
	using namespace PlanningPreflight;
	using namespace StateSemantic;
	using UE::Geometry::FDynamicMesh3;
	using UE::Geometry::FDynamicMeshAttributeSet;
	using UE::Geometry::FDynamicMeshMaterialAttribute;
	using UE::Geometry::FDynamicMeshNormalOverlay;
	using UE::Geometry::FDynamicMeshUVOverlay;
	using UE::Geometry::FIndex2i;
	using UE::Geometry::FIndex3i;

	namespace
	{
		constexpr int32 MaximumVoxelBooleanResolutionPerMajorAxis = 256;

		enum class ECarrierKind : uint8
		{
			PrimitiveOutput,
			DynamicMeshActor,
			StaticMeshAsset,
		};

		struct FModelingGeometryAction
		{
			ECarrierKind Carrier = ECarrierKind::DynamicMeshActor;
			FString OperationKey;
			FString TargetRef;
			FString OperandRef;
			FString CanonicalKey;
			FUeAgentCanonicalValuePtr Parameters;
			TWeakObjectPtr<ADynamicMeshActor> DynamicActor;
			TWeakObjectPtr<UStaticMesh> StaticMesh;
			TWeakObjectPtr<ADynamicMeshActor> CreatedActor;
			TWeakObjectPtr<UPackage> Package;
			FDynamicMesh3 OriginalMesh;
			FDynamicMesh3 WorkingMesh;
			FDynamicMesh3 OperandMesh;
			TArray<TObjectPtr<UMaterialInterface>>
				OriginalDynamicMaterials;
			TArray<FStaticMaterial> OriginalStaticMaterials;
			FKAggregateGeom OriginalCollision;
			bool bHadCollision = false;
			bool bPackageWasDirty = false;
			FTransform Placement = FTransform::Identity;
		};

		bool Fail(
			FExecutionMutationError& OutError,
			FString Code,
			FString Detail = FString())
		{
			OutError = FExecutionMutationError(
				MoveTemp(Code),
				MoveTemp(Detail));
			return false;
		}

		FUeAgentCanonicalValuePtr FindField(
			const FUeAgentCanonicalValuePtr& Object,
			const FString& Field)
		{
			if (!Object.IsValid()
				|| Object->GetKind() != EUeAgentValueKind::Object)
			{
				return nullptr;
			}
			for (const TPair<
				FString,
				FUeAgentCanonicalValueRef>& Item
					: Object->GetObjectItems())
			{
				if (Item.Key == Field)
				{
					return FUeAgentCanonicalValuePtr(Item.Value);
				}
			}
			return nullptr;
		}

		bool ReadString(
			const FUeAgentCanonicalValuePtr& Object,
			const FString& Field,
			FString& OutValue,
			FExecutionMutationError& OutError)
		{
			OutValue.Reset();
			const FUeAgentCanonicalValuePtr Value =
				FindField(Object, Field);
			if (!Value.IsValid()
				|| Value->GetKind() != EUeAgentValueKind::String)
			{
				return Fail(
					OutError,
					TEXT("modeling_geometry_internal_field_type"),
					Field);
			}
			OutValue = Value->GetText();
			return !OutValue.IsEmpty()
				|| Fail(
					OutError,
					TEXT("modeling_geometry_internal_field_empty"),
					Field);
		}

		bool ReadNumber(
			const FUeAgentCanonicalValuePtr& Object,
			const FString& Field,
			double& OutValue)
		{
			OutValue = 0.0;
			const FUeAgentCanonicalValuePtr Value =
				FindField(Object, Field);
			if (!Value.IsValid()
				|| (Value->GetKind() != EUeAgentValueKind::Decimal
					&& Value->GetKind()
						!= EUeAgentValueKind::Integer))
			{
				return false;
			}
			OutValue = FCString::Atod(*Value->GetLexeme());
			return FMath::IsFinite(OutValue);
		}

		bool ReadInteger(
			const FUeAgentCanonicalValuePtr& Object,
			const FString& Field,
			int32& OutValue)
		{
			double Number = 0.0;
			if (!ReadNumber(Object, Field, Number)
				|| Number < static_cast<double>(MIN_int32)
				|| Number > static_cast<double>(MAX_int32))
			{
				return false;
			}
			OutValue = FMath::RoundToInt(Number);
			return FMath::IsNearlyEqual(
				Number,
				static_cast<double>(OutValue),
				UE_DOUBLE_SMALL_NUMBER);
		}

		bool ReadElementId(
			const FUeAgentCanonicalValueRef& Value,
			int32& OutValue)
		{
			if (Value->GetKind() != EUeAgentValueKind::Integer
				&& Value->GetKind() != EUeAgentValueKind::Decimal)
			{
				return false;
			}
			const double Number =
				FCString::Atod(*Value->GetLexeme());
			if (!FMath::IsFinite(Number)
				|| Number < 0.0
				|| Number > static_cast<double>(MAX_int32))
			{
				return false;
			}
			OutValue = FMath::RoundToInt(Number);
			return FMath::IsNearlyEqual(
				Number,
				static_cast<double>(OutValue),
				UE_DOUBLE_SMALL_NUMBER);
		}

		bool ReadBoolean(
			const FUeAgentCanonicalValuePtr& Object,
			const FString& Field,
			bool& OutValue)
		{
			const FUeAgentCanonicalValuePtr Value =
				FindField(Object, Field);
			if (!Value.IsValid()
				|| Value->GetKind() != EUeAgentValueKind::Boolean)
			{
				return false;
			}
			OutValue = Value->GetBoolean();
			return true;
		}

		bool ReadNumberArray(
			const FUeAgentCanonicalValuePtr& Object,
			const FString& Field,
			const int32 Count,
			TArray<double>& OutValues)
		{
			OutValues.Reset();
			const FUeAgentCanonicalValuePtr Value =
				FindField(Object, Field);
			if (!Value.IsValid()
				|| Value->GetKind() != EUeAgentValueKind::Array
				|| Value->GetArrayItems().Num() != Count)
			{
				return false;
			}
			for (const FUeAgentCanonicalValueRef& Item
				: Value->GetArrayItems())
			{
				if (Item->GetKind() != EUeAgentValueKind::Integer
					&& Item->GetKind() != EUeAgentValueKind::Decimal)
				{
					return false;
				}
				const double Number =
					FCString::Atod(*Item->GetLexeme());
				if (!FMath::IsFinite(Number))
				{
					return false;
				}
				OutValues.Add(Number);
			}
			return true;
		}

		FString MeshFingerprint(const FDynamicMesh3& Mesh)
		{
			int64 Checksum = 0;
			for (const int32 VertexId : Mesh.VertexIndicesItr())
			{
				const FVector3d Position = Mesh.GetVertex(VertexId);
				Checksum +=
					FMath::RoundToInt64(Position.X * 100.0)
					+ FMath::RoundToInt64(Position.Y * 1000.0)
					+ FMath::RoundToInt64(Position.Z * 10000.0);
			}
			return FString::Printf(
				TEXT("mesh:v=%d:t=%d:e=%d:c=%lld"),
				Mesh.VertexCount(),
				Mesh.TriangleCount(),
				Mesh.EdgeCount(),
				Checksum);
		}

		bool ReadIntegerArray(
			const FUeAgentCanonicalValuePtr& Object,
			const FString& Field,
			const int32 Count,
			TArray<int32>& OutValues)
		{
			TArray<double> Numbers;
			if (!ReadNumberArray(Object, Field, Count, Numbers))
			{
				return false;
			}
			OutValues.Reset(Count);
			for (const double Number : Numbers)
			{
				const int32 Value = FMath::RoundToInt(Number);
				if (!FMath::IsNearlyEqual(
						Number,
						static_cast<double>(Value),
						UE_DOUBLE_SMALL_NUMBER))
				{
					return false;
				}
				OutValues.Add(Value);
			}
			return true;
		}

		bool ReadElementIds(
			const FUeAgentCanonicalValuePtr& Object,
			const FString& Field,
			TArray<int32>& OutValues)
		{
			OutValues.Reset();
			const FUeAgentCanonicalValuePtr Value =
				FindField(Object, Field);
			if (!Value.IsValid()
				|| Value->GetKind() != EUeAgentValueKind::Array
				|| Value->GetArrayItems().IsEmpty())
			{
				return false;
			}
			for (const FUeAgentCanonicalValueRef& Item
				: Value->GetArrayItems())
			{
				int32 Parsed = INDEX_NONE;
				if (!ReadElementId(Item, Parsed))
				{
					return false;
				}
				OutValues.Add(Parsed);
			}
			return true;
		}

		TStrongObjectPtr<UDynamicMesh> MakeTransientMesh(
			const FDynamicMesh3& Mesh)
		{
			TStrongObjectPtr<UDynamicMesh> Result(
				NewObject<UDynamicMesh>(
					GetTransientPackage(),
					NAME_None,
					RF_Transient));
			if (Result.IsValid())
			{
				Result->SetMesh(Mesh);
			}
			return Result;
		}

		void EnsureTopologyVertexNormals(FDynamicMesh3& Mesh)
		{
			if (!Mesh.HasVertexNormals())
			{
				UE::Geometry::FMeshNormals::QuickComputeVertexNormals(Mesh);
			}
		}

		bool CopyTransientMesh(
			const UDynamicMesh& Source,
			FDynamicMesh3& OutMesh,
			FExecutionMutationError& OutError)
		{
			bool bCopied = false;
			Source.ProcessMesh(
				[&OutMesh, &bCopied](const FDynamicMesh3& Mesh)
				{
					OutMesh = Mesh;
					bCopied = true;
				});
			return (bCopied
					&& OutMesh.TriangleCount() > 0
					&& OutMesh.VertexCount() > 0)
				|| Fail(
					OutError,
					TEXT("modeling_geometry_kernel_result_invalid"));
		}

		bool CutMeshHalf(
			FDynamicMesh3& Mesh,
			const FVector3d& Origin,
			const FVector3d& Normal,
			const bool bKeepPositiveSide,
			const bool bFill)
		{
			UE::Geometry::FMeshPlaneCut Cut(
				&Mesh,
				Origin,
				bKeepPositiveSide ? -Normal : Normal);
			if (!Cut.Cut())
			{
				return false;
			}
			return !bFill || Cut.MinimalHoleFill();
		}

		bool ReadTransform(
			const FUeAgentCanonicalValuePtr& Object,
			const FString& TranslationField,
			const FString& RotationField,
			const FString& ScaleField,
			FTransform& OutTransform)
		{
			TArray<double> Translation;
			TArray<double> Rotation;
			TArray<double> Scale;
			if (!ReadNumberArray(
					Object,
					TranslationField,
					3,
					Translation)
				|| !ReadNumberArray(
					Object,
					RotationField,
					4,
					Rotation)
				|| !ReadNumberArray(
					Object,
					ScaleField,
					3,
					Scale))
			{
				return false;
			}
			double UnitScale = 1.0;
			const FUeAgentCanonicalValuePtr Unit =
				FindField(Object, TEXT("unit"));
			if (Unit.IsValid()
				&& Unit->GetKind() == EUeAgentValueKind::String)
			{
				if (Unit->GetText() == TEXT("meter"))
				{
					UnitScale = 100.0;
				}
				else if (Unit->GetText() != TEXT("centimeter"))
				{
					return false;
				}
			}
			FQuat Quaternion(
				Rotation[0],
				Rotation[1],
				Rotation[2],
				Rotation[3]);
			if (Quaternion.SizeSquared() <= UE_DOUBLE_SMALL_NUMBER)
			{
				return false;
			}
			Quaternion.Normalize();
			OutTransform = FTransform(
				Quaternion,
				FVector(
					Translation[0] * UnitScale,
					Translation[1] * UnitScale,
					Translation[2] * UnitScale),
				FVector(Scale[0], Scale[1], Scale[2]));
			return true;
		}

		bool ReadPlane(
			const FUeAgentCanonicalValuePtr& Parameters,
			FVector3d& OutOrigin,
			FVector3d& OutNormal)
		{
			const FUeAgentCanonicalValuePtr Plane =
				FindField(Parameters, TEXT("plane"));
			TArray<double> Origin;
			TArray<double> Normal;
			if (!Plane.IsValid()
				|| !ReadNumberArray(
					Plane,
					TEXT("origin"),
					3,
					Origin)
				|| !ReadNumberArray(
					Plane,
					TEXT("normal"),
					3,
					Normal))
			{
				return false;
			}
			double UnitScale = 1.0;
			const FUeAgentCanonicalValuePtr Unit =
				FindField(Plane, TEXT("unit"));
			if (Unit.IsValid()
				&& Unit->GetKind() == EUeAgentValueKind::String
				&& Unit->GetText() == TEXT("meter"))
			{
				UnitScale = 100.0;
			}
			OutOrigin = FVector3d(
				Origin[0] * UnitScale,
				Origin[1] * UnitScale,
				Origin[2] * UnitScale);
			OutNormal =
				FVector3d(Normal[0], Normal[1], Normal[2]);
			return OutNormal.Normalize();
		}

		bool ReadDirection(
			const FUeAgentCanonicalValuePtr& Parameters,
			FVector3d& OutDirection)
		{
			const FUeAgentCanonicalValuePtr Direction =
				FindField(Parameters, TEXT("direction"));
			TArray<double> Vector;
			if (!Direction.IsValid()
				|| !ReadNumberArray(
					Direction,
					TEXT("vector"),
					3,
					Vector))
			{
				return false;
			}
			OutDirection =
				FVector3d(Vector[0], Vector[1], Vector[2]);
			return OutDirection.Normalize();
		}

		bool BuildSelection(
			UDynamicMesh& DynamicMesh,
			const FUeAgentCanonicalValuePtr& Parameters,
			FGeometryScriptMeshSelection& OutSelection,
			FExecutionMutationError& OutError)
		{
			const FUeAgentCanonicalValuePtr Selection =
				FindField(Parameters, TEXT("selection"));
			FString Kind;
			TArray<int32> ElementIds;
			if (!ReadString(
					Selection,
					TEXT("element_kind"),
					Kind,
					OutError)
				|| !ReadElementIds(
					Selection,
					TEXT("element_ids"),
					ElementIds))
			{
				return Fail(
					OutError,
					TEXT("modeling_geometry_selection_invalid"));
			}
			const EGeometryScriptMeshSelectionType SelectionType =
				Kind == TEXT("triangle")
					? EGeometryScriptMeshSelectionType::Triangles
					: Kind == TEXT("vertex")
						? EGeometryScriptMeshSelectionType::Vertices
						: EGeometryScriptMeshSelectionType::Edges;
			bool bValid = true;
			DynamicMesh.ProcessMesh(
				[&](const FDynamicMesh3& Mesh)
				{
					for (const int32 ElementId : ElementIds)
					{
						bValid = bValid
							&& (SelectionType
									== EGeometryScriptMeshSelectionType::
										Triangles
								? Mesh.IsTriangle(ElementId)
								: SelectionType
										== EGeometryScriptMeshSelectionType::
											Vertices
									? Mesh.IsVertex(ElementId)
									: Mesh.IsEdge(ElementId));
						if (Kind == TEXT("boundary_edge"))
						{
							bValid = bValid
								&& Mesh.IsBoundaryEdge(ElementId);
						}
					}
				});
			if (!bValid)
			{
				return Fail(
					OutError,
					TEXT("modeling_geometry_selection_stale"));
			}
			UGeometryScriptLibrary_MeshSelectionFunctions::
				ConvertIndexArrayToMeshSelection(
					&DynamicMesh,
					ElementIds,
					SelectionType,
					OutSelection);
			return !OutSelection.IsEmpty()
				|| Fail(
					OutError,
					TEXT("modeling_geometry_selection_empty"));
		}

		void AppendBox(
			FDynamicMesh3& Mesh,
			const FVector3d& Center,
			const FVector3d& Size)
		{
			const FVector3d E = Size * 0.5;
			const int32 V0 = Mesh.AppendVertex(
				Center + FVector3d(-E.X, -E.Y, -E.Z));
			const int32 V1 = Mesh.AppendVertex(
				Center + FVector3d(E.X, -E.Y, -E.Z));
			const int32 V2 = Mesh.AppendVertex(
				Center + FVector3d(E.X, E.Y, -E.Z));
			const int32 V3 = Mesh.AppendVertex(
				Center + FVector3d(-E.X, E.Y, -E.Z));
			const int32 V4 = Mesh.AppendVertex(
				Center + FVector3d(-E.X, -E.Y, E.Z));
			const int32 V5 = Mesh.AppendVertex(
				Center + FVector3d(E.X, -E.Y, E.Z));
			const int32 V6 = Mesh.AppendVertex(
				Center + FVector3d(E.X, E.Y, E.Z));
			const int32 V7 = Mesh.AppendVertex(
				Center + FVector3d(-E.X, E.Y, E.Z));
			for (const FIndex3i Triangle
				: {
					FIndex3i(V0, V2, V1),
					FIndex3i(V0, V3, V2),
					FIndex3i(V4, V5, V6),
					FIndex3i(V4, V6, V7),
					FIndex3i(V0, V1, V5),
					FIndex3i(V0, V5, V4),
					FIndex3i(V1, V2, V6),
					FIndex3i(V1, V6, V5),
					FIndex3i(V2, V3, V7),
					FIndex3i(V2, V7, V6),
					FIndex3i(V3, V0, V4),
					FIndex3i(V3, V4, V7),
				})
			{
				Mesh.AppendTriangle(Triangle);
			}
		}

		void AppendRamp(
			FDynamicMesh3& Mesh,
			const FVector3d& Size,
			const bool bRiseAlongX,
			const bool bPositive)
		{
			const double RunExtent =
				(bRiseAlongX ? Size.X : Size.Y) * 0.5;
			const double WidthExtent =
				(bRiseAlongX ? Size.Y : Size.X) * 0.5;
			const double HighRun =
				bPositive ? RunExtent : -RunExtent;
			const double LowRun = -HighRun;
			auto Position =
				[bRiseAlongX](
					const double Run,
					const double Width,
					const double Z)
				{
					return bRiseAlongX
						? FVector3d(Run, Width, Z)
						: FVector3d(Width, Run, Z);
				};
			const int32 H0 = Mesh.AppendVertex(
				Position(HighRun, -WidthExtent, 0.0));
			const int32 H1 = Mesh.AppendVertex(
				Position(HighRun, WidthExtent, 0.0));
			const int32 L0 = Mesh.AppendVertex(
				Position(LowRun, -WidthExtent, 0.0));
			const int32 L1 = Mesh.AppendVertex(
				Position(LowRun, WidthExtent, 0.0));
			const int32 T0 = Mesh.AppendVertex(
				Position(HighRun, -WidthExtent, Size.Z));
			const int32 T1 = Mesh.AppendVertex(
				Position(HighRun, WidthExtent, Size.Z));
			for (const FIndex3i Triangle
				: {
					FIndex3i(H0, L1, L0),
					FIndex3i(H0, H1, L1),
					FIndex3i(H0, T0, T1),
					FIndex3i(H0, T1, H1),
					FIndex3i(T0, L0, L1),
					FIndex3i(T0, L1, T1),
					FIndex3i(H0, L0, T0),
					FIndex3i(H1, T1, L1),
				})
			{
				Mesh.AppendTriangle(Triangle);
			}
		}

		void AppendRampCorner(
			FDynamicMesh3& Mesh,
			const FVector3d& Size,
			const FString& HighCorner)
		{
			const double X = Size.X * 0.5;
			const double Y = Size.Y * 0.5;
			const int32 A = Mesh.AppendVertex(
				FVector3d(-X, -Y, 0.0));
			const int32 B = Mesh.AppendVertex(
				FVector3d(X, -Y, 0.0));
			const int32 C = Mesh.AppendVertex(
				FVector3d(X, Y, 0.0));
			const int32 D = Mesh.AppendVertex(
				FVector3d(-X, Y, 0.0));
			const double HighX =
				HighCorner.StartsWith(TEXT("negative_x"))
					? -X
					: X;
			const double HighY =
				HighCorner.EndsWith(TEXT("negative_y"))
					? -Y
					: Y;
			const int32 E = Mesh.AppendVertex(
				FVector3d(HighX, HighY, Size.Z));
			for (const FIndex3i Triangle
				: {
					FIndex3i(A, C, B),
					FIndex3i(A, D, C),
					FIndex3i(A, B, E),
					FIndex3i(B, C, E),
					FIndex3i(C, D, E),
					FIndex3i(D, A, E),
				})
			{
				Mesh.AppendTriangle(Triangle);
			}
		}

		bool GeneratePrimitive(
			const FString& OperationKey,
			const FUeAgentCanonicalValuePtr& Parameters,
			FDynamicMesh3& Mesh,
			FExecutionMutationError& OutError)
		{
			Mesh.Clear();
			Mesh.EnableTriangleGroups();
			FGeometryScriptPrimitiveOptions PrimitiveOptions;
			PrimitiveOptions.PolygroupMode =
				EGeometryScriptPrimitivePolygroupMode::PerFace;

			if (OperationKey == TEXT("ramp")
				|| OperationKey == TEXT("ramp_corner"))
			{
				TArray<double> SizeValues;
				if (!ReadNumberArray(
						Parameters,
						TEXT("size_cm"),
						3,
						SizeValues))
				{
					return Fail(
						OutError,
						TEXT("modeling_geometry_primitive_parameters_invalid"));
				}
				const FVector3d Size(
					SizeValues[0],
					SizeValues[1],
					SizeValues[2]);
				if (OperationKey == TEXT("ramp"))
				{
					FString SlopeAxis;
					FString RiseDirection;
					if (!ReadString(
							Parameters,
							TEXT("slope_axis"),
							SlopeAxis,
							OutError)
						|| !ReadString(
							Parameters,
							TEXT("rise_direction"),
							RiseDirection,
							OutError))
					{
						return false;
					}
					AppendRamp(
						Mesh,
						Size,
						SlopeAxis == TEXT("x"),
						RiseDirection == TEXT("positive"));
				}
				else
				{
					FString HighCorner;
					if (!ReadString(
							Parameters,
							TEXT("high_corner"),
							HighCorner,
							OutError))
					{
						return false;
					}
					AppendRampCorner(Mesh, Size, HighCorner);
				}
				Mesh.ReverseOrientation(false);
				Mesh.EnableAttributes();
				return true;
			}

			if (OperationKey == TEXT("stairs"))
			{
				int32 StepCount = 0;
				double Width = 0.0;
				double Depth = 0.0;
				double Height = 0.0;
				FString RiseAxis;
				FString RiseDirection;
				if (!ReadInteger(
						Parameters,
						TEXT("step_count"),
						StepCount)
					|| !ReadNumber(
						Parameters,
						TEXT("step_width_cm"),
						Width)
					|| !ReadNumber(
						Parameters,
						TEXT("step_depth_cm"),
						Depth)
					|| !ReadNumber(
						Parameters,
						TEXT("step_height_cm"),
						Height)
					|| !ReadString(
						Parameters,
						TEXT("rise_axis"),
						RiseAxis,
						OutError)
					|| !ReadString(
						Parameters,
						TEXT("rise_direction"),
						RiseDirection,
						OutError))
				{
					return Fail(
						OutError,
						TEXT("modeling_geometry_primitive_parameters_invalid"));
				}
				const double Sign =
					RiseDirection == TEXT("positive") ? 1.0 : -1.0;
				for (int32 Index = 0; Index < StepCount; ++Index)
				{
					const double RunCenter =
						Sign * (Index + 0.5) * Depth;
					const double StepHeight = (Index + 1) * Height;
					AppendBox(
						Mesh,
						RiseAxis == TEXT("x")
							? FVector3d(
								RunCenter,
								0.0,
								StepHeight * 0.5)
							: FVector3d(
								0.0,
								RunCenter,
								StepHeight * 0.5),
						RiseAxis == TEXT("x")
							? FVector3d(
								Depth,
								Width,
								StepHeight)
							: FVector3d(
								Width,
								Depth,
								StepHeight));
				}
				Mesh.ReverseOrientation(false);
				Mesh.EnableAttributes();
				return true;
			}

			TStrongObjectPtr<UDynamicMesh> DynamicMesh =
				MakeTransientMesh(Mesh);
			if (!DynamicMesh.IsValid())
			{
				return Fail(
					OutError,
					TEXT("modeling_geometry_transient_mesh_failed"));
			}
			if (OperationKey == TEXT("box"))
			{
				TArray<double> Size;
				TArray<int32> Subdivisions;
				FString Anchor;
				if (!ReadNumberArray(
						Parameters,
						TEXT("size_cm"),
						3,
						Size)
					|| !ReadIntegerArray(
						Parameters,
						TEXT("subdivisions"),
						3,
						Subdivisions)
					|| !ReadString(
						Parameters,
						TEXT("anchor"),
						Anchor,
						OutError))
				{
					return Fail(
						OutError,
						TEXT("modeling_geometry_primitive_parameters_invalid"));
				}
				UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
					DynamicMesh.Get(),
					PrimitiveOptions,
					FTransform::Identity,
					Size[0],
					Size[1],
					Size[2],
					Subdivisions[0] + 1,
					Subdivisions[1] + 1,
					Subdivisions[2] + 1,
					Anchor == TEXT("center")
						? EGeometryScriptPrimitiveOriginMode::Center
						: EGeometryScriptPrimitiveOriginMode::Base);
			}
			else if (OperationKey == TEXT("cylinder"))
			{
				double Radius = 0.0;
				double Height = 0.0;
				int32 RadialSegments = 0;
				int32 HeightSegments = 0;
				bool bCapped = false;
				FString Anchor;
				if (!ReadNumber(
						Parameters,
						TEXT("radius_cm"),
						Radius)
					|| !ReadNumber(
						Parameters,
						TEXT("height_cm"),
						Height)
					|| !ReadInteger(
						Parameters,
						TEXT("radial_segments"),
						RadialSegments)
					|| !ReadInteger(
						Parameters,
						TEXT("height_segments"),
						HeightSegments)
					|| !ReadBoolean(
						Parameters,
						TEXT("cap_ends"),
						bCapped)
					|| !ReadString(
						Parameters,
						TEXT("anchor"),
						Anchor,
						OutError))
				{
					return Fail(
						OutError,
						TEXT("modeling_geometry_primitive_parameters_invalid"));
				}
				UGeometryScriptLibrary_MeshPrimitiveFunctions::
					AppendCylinder(
						DynamicMesh.Get(),
						PrimitiveOptions,
						FTransform::Identity,
						Radius,
						Height,
						RadialSegments,
						FMath::Max(0, HeightSegments - 1),
						bCapped,
						Anchor == TEXT("center")
							? EGeometryScriptPrimitiveOriginMode::Center
							: EGeometryScriptPrimitiveOriginMode::Base);
			}
			else if (OperationKey == TEXT("plane"))
			{
				TArray<double> Size;
				TArray<int32> Subdivisions;
				bool bDoubleSided = false;
				if (!ReadNumberArray(
						Parameters,
						TEXT("size_cm"),
						2,
						Size)
					|| !ReadIntegerArray(
						Parameters,
						TEXT("subdivisions"),
						2,
						Subdivisions)
					|| !ReadBoolean(
						Parameters,
						TEXT("double_sided"),
						bDoubleSided))
				{
					return Fail(
						OutError,
						TEXT("modeling_geometry_primitive_parameters_invalid"));
				}
				UGeometryScriptLibrary_MeshPrimitiveFunctions::
					AppendRectangleXY(
						DynamicMesh.Get(),
						PrimitiveOptions,
						FTransform::Identity,
						Size[0],
						Size[1],
						Subdivisions[0] + 1,
						Subdivisions[1] + 1);
				if (bDoubleSided)
				{
					PrimitiveOptions.bFlipOrientation = true;
					UGeometryScriptLibrary_MeshPrimitiveFunctions::
						AppendRectangleXY(
							DynamicMesh.Get(),
							PrimitiveOptions,
							FTransform::Identity,
							Size[0],
							Size[1],
							Subdivisions[0] + 1,
							Subdivisions[1] + 1);
				}
			}
			else if (OperationKey == TEXT("sphere"))
			{
				double Radius = 0.0;
				int32 LongitudeSegments = 0;
				int32 LatitudeSegments = 0;
				if (!ReadNumber(
						Parameters,
						TEXT("radius_cm"),
						Radius)
					|| !ReadInteger(
						Parameters,
						TEXT("longitude_segments"),
						LongitudeSegments)
					|| !ReadInteger(
						Parameters,
						TEXT("latitude_segments"),
						LatitudeSegments))
				{
					return Fail(
						OutError,
						TEXT("modeling_geometry_primitive_parameters_invalid"));
				}
				UGeometryScriptLibrary_MeshPrimitiveFunctions::
					AppendSphereLatLong(
						DynamicMesh.Get(),
						PrimitiveOptions,
						FTransform::Identity,
						Radius,
						LatitudeSegments
							+ ((LatitudeSegments % 2) == 0 ? 1 : 0),
						LongitudeSegments,
						EGeometryScriptPrimitiveOriginMode::Center);
			}
			else
			{
				return Fail(
					OutError,
					TEXT(
						"modeling_geometry_primitive_operation_unsupported"),
					OperationKey);
			}
			return CopyTransientMesh(
				*DynamicMesh,
				Mesh,
				OutError);
		}

		void ApplySimplifyWithBoundaryPolicy(
			FDynamicMesh3& Mesh,
			const int32 TriangleTarget,
			const double GeometricTolerance,
			const bool bUseTolerance,
			const bool bPreserveBoundaries)
		{
			FDynamicMesh3 ProjectionMesh;
			TUniquePtr<UE::Geometry::FDynamicMeshAABBTree3> Spatial;
			TUniquePtr<UE::Geometry::FMeshProjectionTarget> ProjectionTarget;
			if (bUseTolerance)
			{
				ProjectionMesh.Copy(Mesh, false, false, false, false);
				Spatial = MakeUnique<UE::Geometry::FDynamicMeshAABBTree3>(
					&ProjectionMesh,
					true);
				ProjectionTarget =
					MakeUnique<UE::Geometry::FMeshProjectionTarget>(
						&ProjectionMesh,
						Spatial.Get());
			}

			UE::Geometry::FAttrMeshSimplification Simplifier(&Mesh);
			Simplifier.ProjectionMode = UE::Geometry::FAttrMeshSimplification::
				ETargetProjectionMode::NoProjection;
			if (ProjectionTarget)
			{
				Simplifier.SetProjectionTarget(ProjectionTarget.Get());
			}
			Simplifier.DEBUG_CHECK_LEVEL = 0;
			Simplifier.bRetainQuadricMemory = false;
			Simplifier.bAllowSeamCollapse = true;
			Simplifier.SetEdgeFlipTolerance(1.e-5);
			if (Mesh.HasAttributes())
			{
				Mesh.Attributes()->SplitAllBowties();
			}

			UE::Geometry::FMeshConstraints Constraints;
			UE::Geometry::FMeshConstraintsUtil::
				ConstrainAllBoundariesAndSeams(
					Constraints,
					Mesh,
					bPreserveBoundaries
						? UE::Geometry::EEdgeRefineFlags::FullyConstrained
						: UE::Geometry::EEdgeRefineFlags::NoFlip,
					UE::Geometry::EEdgeRefineFlags::NoConstraint,
					UE::Geometry::EEdgeRefineFlags::NoConstraint,
					true,
					true,
					true);
			Simplifier.SetExternalConstraints(MoveTemp(Constraints));
			if (bUseTolerance)
			{
				Simplifier.GeometricErrorConstraint =
					UE::Geometry::FAttrMeshSimplification::
						EGeometricErrorCriteria::
							PredictedPointToProjectionTarget;
				Simplifier.GeometricErrorTolerance =
					FMath::Max(0.0, GeometricTolerance);
			}
			Simplifier.SimplifyToTriangleCount(FMath::Max(1, TriangleTarget));
			Mesh.CompactInPlace();
		}

		bool ApplyUniformRemeshWithPolicy(
			FDynamicMesh3& Mesh,
			const double TargetEdgeLength,
			const int32 Iterations,
			const bool bPreserveBoundaries,
			const bool bPreserveSharpEdges)
		{
			TSharedPtr<FDynamicMesh3, ESPMode::ThreadSafe> SourceMesh =
				MakeShared<FDynamicMesh3, ESPMode::ThreadSafe>(MoveTemp(Mesh));
			TSharedPtr<
				UE::Geometry::FDynamicMeshAABBTree3,
				ESPMode::ThreadSafe> SourceSpatial =
					MakeShared<
						UE::Geometry::FDynamicMeshAABBTree3,
						ESPMode::ThreadSafe>(
							SourceMesh.Get(),
							true);

			UE::Geometry::FRemeshMeshOp Remesh;
			Remesh.OriginalMesh = SourceMesh;
			Remesh.OriginalMeshSpatial = SourceSpatial;
			Remesh.RemeshType = ERemeshType::Standard;
			Remesh.RemeshIterations = Iterations;
			Remesh.MaxRemeshIterations = Iterations;
			Remesh.ExtraProjectionIterations = 0;
			Remesh.TriangleCountHint = 0;
			Remesh.TargetEdgeLength = TargetEdgeLength;
			Remesh.SmoothingStrength = 0.25f;
			Remesh.SmoothingType = ERemeshSmoothingType::MeanValue;
			Remesh.bDiscardAttributes = false;
			Remesh.bPreserveSharpEdges = bPreserveSharpEdges;
			Remesh.bFlips = true;
			Remesh.bSplits = true;
			Remesh.bCollapses = true;
			Remesh.bReproject = true;
			Remesh.bPreventNormalFlips = true;
			Remesh.bPreventTinyTriangles = true;
			Remesh.MeshBoundaryConstraint = bPreserveBoundaries
				? UE::Geometry::EEdgeRefineFlags::FullyConstrained
				: UE::Geometry::EEdgeRefineFlags::NoFlip;
			Remesh.GroupBoundaryConstraint =
				UE::Geometry::EEdgeRefineFlags::NoFlip;
			Remesh.MaterialBoundaryConstraint =
				UE::Geometry::EEdgeRefineFlags::NoFlip;
			Remesh.bReprojectConstraints = false;
			Remesh.BoundaryCornerAngleThreshold = 45.0;
			Remesh.TargetMeshLocalToWorld =
				UE::Geometry::FTransformSRT3d::Identity();
			Remesh.ToolMeshLocalToWorld =
				UE::Geometry::FTransformSRT3d::Identity();
			Remesh.bUseWorldSpace = false;
			Remesh.bParallel = true;
			Remesh.CalculateResult(nullptr);

			TUniquePtr<FDynamicMesh3> Result = Remesh.ExtractResult();
			if (!Result.IsValid() || Result->TriangleCount() == 0)
			{
				Mesh = MoveTemp(*SourceMesh);
				return false;
			}
			Mesh = MoveTemp(*Result);
			Mesh.CompactInPlace();
			return true;
		}

		bool ApplyTopologyOperation(
			FModelingGeometryAction& Action,
			FExecutionMutationError& OutError)
		{
			const FString& Key = Action.OperationKey;
			if (Action.WorkingMesh.TriangleCount() == 0)
			{
				return Fail(
					OutError,
					TEXT("modeling_geometry_mesh_empty"));
			}

			if (Key == TEXT("fill_holes"))
			{
				const FUeAgentCanonicalValuePtr Selection =
					FindField(Action.Parameters, TEXT("selection"));
				TArray<int32> SelectedEdges;
				FString FillMode;
				if (!ReadElementIds(
						Selection,
						TEXT("element_ids"),
						SelectedEdges)
					|| !ReadString(
						Action.Parameters,
						TEXT("fill_mode"),
						FillMode,
						OutError))
				{
					return Fail(
						OutError,
						TEXT("modeling_geometry_fill_parameters_invalid"));
				}
				TSharedPtr<FDynamicMesh3> Original =
					MakeShared<FDynamicMesh3>(Action.WorkingMesh);
				UE::Geometry::FMeshBoundaryLoops BoundaryLoops(
					Original.Get(),
					true);
				if (BoundaryLoops.bAborted)
				{
					return Fail(
						OutError,
						TEXT("modeling_geometry_boundary_loops_invalid"));
				}
				TSet<int32> LoopIndices;
				for (const int32 EdgeId : SelectedEdges)
				{
					if (!Original->IsEdge(EdgeId)
						|| !Original->IsBoundaryEdge(EdgeId))
					{
						return Fail(
							OutError,
							TEXT(
								"modeling_geometry_boundary_selection_stale"));
					}
					const int32 LoopIndex =
						BoundaryLoops.FindLoopContainingEdge(EdgeId);
					if (LoopIndex == INDEX_NONE)
					{
						return Fail(
							OutError,
							TEXT(
								"modeling_geometry_boundary_loop_missing"));
					}
					LoopIndices.Add(LoopIndex);
				}
				UE::Geometry::FHoleFillOp HoleFiller;
				HoleFiller.OriginalMesh = Original;
				for (const int32 LoopIndex : LoopIndices)
				{
					HoleFiller.Loops.Add(
						BoundaryLoops.Loops[LoopIndex]);
				}
				HoleFiller.FillType =
					FillMode == TEXT("triangle_fan")
						? EHoleFillOpFillType::TriangleFan
						: FillMode == TEXT("planar")
							? EHoleFillOpFillType::Planar
							: EHoleFillOpFillType::Minimal;
				HoleFiller.FillOptions.bQuickFillSmallHoles = true;
				HoleFiller.CalculateResult(nullptr);
				TUniquePtr<FDynamicMesh3> Result =
					HoleFiller.ExtractResult();
				if (!Result.IsValid()
					|| HoleFiller.NumFailedLoops > 0)
				{
					return Fail(
						OutError,
						TEXT("modeling_geometry_hole_fill_failed"));
				}
				Action.WorkingMesh = MoveTemp(*Result);
				return true;
			}

			if (Key == TEXT("weld_edges"))
			{
				double Tolerance = 0.0;
				const FUeAgentCanonicalValuePtr Pairs =
					FindField(Action.Parameters, TEXT("edge_pairs"));
				if (!ReadNumber(
						Action.Parameters,
						TEXT("tolerance_cm"),
						Tolerance)
					|| !Pairs.IsValid()
					|| Pairs->GetKind() != EUeAgentValueKind::Array
					|| Pairs->GetArrayItems().IsEmpty())
				{
					return Fail(
						OutError,
						TEXT("modeling_geometry_weld_parameters_invalid"));
				}
				for (const FUeAgentCanonicalValueRef& PairValue
					: Pairs->GetArrayItems())
				{
					if (PairValue->GetKind()
							!= EUeAgentValueKind::Array
						|| PairValue->GetArrayItems().Num() != 2)
					{
						return Fail(
							OutError,
							TEXT(
								"modeling_geometry_weld_pair_invalid"));
					}
					const FUeAgentCanonicalValueRef& AValue =
						PairValue->GetArrayItems()[0];
					const FUeAgentCanonicalValueRef& BValue =
						PairValue->GetArrayItems()[1];
					int32 EdgeA = INDEX_NONE;
					int32 EdgeB = INDEX_NONE;
					if (!ReadElementId(AValue, EdgeA)
						|| !ReadElementId(BValue, EdgeB))
					{
						return Fail(
							OutError,
							TEXT(
								"modeling_geometry_weld_pair_invalid"));
					}
					if (!Action.WorkingMesh.IsEdge(EdgeA)
						|| !Action.WorkingMesh.IsEdge(EdgeB)
						|| !Action.WorkingMesh.IsBoundaryEdge(EdgeA)
						|| !Action.WorkingMesh.IsBoundaryEdge(EdgeB))
					{
						return Fail(
							OutError,
							TEXT(
								"modeling_geometry_weld_pair_stale"));
					}
					const FIndex2i VerticesA =
						Action.WorkingMesh.GetEdgeV(EdgeA);
					const FIndex2i VerticesB =
						Action.WorkingMesh.GetEdgeV(EdgeB);
					const FVector3d A0 =
						Action.WorkingMesh.GetVertex(VerticesA.A);
					const FVector3d A1 =
						Action.WorkingMesh.GetVertex(VerticesA.B);
					const FVector3d B0 =
						Action.WorkingMesh.GetVertex(VerticesB.A);
					const FVector3d B1 =
						Action.WorkingMesh.GetVertex(VerticesB.B);
					const double DirectDistance = FMath::Max(
						FVector3d::Distance(A0, B0),
						FVector3d::Distance(A1, B1));
					const double ReverseDistance = FMath::Max(
						FVector3d::Distance(A0, B1),
						FVector3d::Distance(A1, B0));
					if (FMath::Min(DirectDistance, ReverseDistance)
						> Tolerance)
					{
						return Fail(
							OutError,
							TEXT(
								"modeling_geometry_weld_tolerance_exceeded"));
					}
					FDynamicMesh3::FMergeEdgesInfo MergeInfo;
					if (Action.WorkingMesh.MergeEdges(
							EdgeA,
							EdgeB,
							0.5,
							MergeInfo,
							true)
						!= UE::Geometry::EMeshResult::Ok)
					{
						return Fail(
							OutError,
							TEXT("modeling_geometry_weld_failed"));
					}
				}
				return true;
			}

			TStrongObjectPtr<UDynamicMesh> TargetMesh =
				MakeTransientMesh(Action.WorkingMesh);
			if (!TargetMesh.IsValid())
			{
				return Fail(
					OutError,
					TEXT("modeling_geometry_transient_mesh_failed"));
			}

			if (Key == TEXT("boolean")
				|| Key == TEXT("mesh_cut")
				|| Key == TEXT("trim"))
			{
				const FUeAgentCanonicalValuePtr OperandTransformValue =
					FindField(
						Action.Parameters,
						TEXT("operand_to_target"));
				FTransform OperandTransform;
				if (!OperandTransformValue.IsValid()
					|| !ReadTransform(
						OperandTransformValue,
						TEXT("translation"),
						TEXT("rotation_quaternion_xyzw"),
						TEXT("scale"),
						OperandTransform))
				{
					return Fail(
						OutError,
						TEXT(
							"modeling_geometry_operand_transform_invalid"));
				}
				TStrongObjectPtr<UDynamicMesh> OperandMesh =
					MakeTransientMesh(Action.OperandMesh);
				if (!OperandMesh.IsValid())
				{
					return Fail(
						OutError,
						TEXT("modeling_geometry_operand_mesh_invalid"));
				}
				FGeometryScriptMeshBooleanOptions Options;
				Options.OutputTransformSpace =
					EGeometryScriptBooleanOutputSpace::
						TargetTransformSpace;
				EGeometryScriptBooleanOperation Operation =
					EGeometryScriptBooleanOperation::Union;
				if (Key == TEXT("boolean"))
				{
					FString RequestedOperation;
					if (!ReadString(
							Action.Parameters,
							TEXT("operation"),
							RequestedOperation,
							OutError)
						|| !ReadBoolean(
							Action.Parameters,
							TEXT("simplify_output"),
							Options.bSimplifyOutput))
					{
						return Fail(
							OutError,
							TEXT(
								"modeling_geometry_boolean_parameters_invalid"));
					}
					Operation =
						RequestedOperation == TEXT("intersection")
							? EGeometryScriptBooleanOperation::Intersection
							: RequestedOperation == TEXT("difference")
								? EGeometryScriptBooleanOperation::Subtract
								: EGeometryScriptBooleanOperation::Union;
				}
				else if (Key == TEXT("mesh_cut"))
				{
					FString KeepSides;
					if (!ReadString(
							Action.Parameters,
							TEXT("keep_sides"),
							KeepSides,
							OutError)
						|| !ReadBoolean(
							Action.Parameters,
							TEXT("fill_cut"),
							Options.bFillHoles))
					{
						return Fail(
							OutError,
							TEXT(
								"modeling_geometry_mesh_cut_parameters_invalid"));
					}
					Operation =
						KeepSides == TEXT("target_inside")
							? EGeometryScriptBooleanOperation::TrimOutside
							: KeepSides == TEXT("target_outside")
								? EGeometryScriptBooleanOperation::TrimInside
								: EGeometryScriptBooleanOperation::
									NewPolyGroupInside;
				}
				else
				{
					FString KeepRegion;
					if (!ReadString(
							Action.Parameters,
							TEXT("keep_region"),
							KeepRegion,
							OutError)
						|| !ReadBoolean(
							Action.Parameters,
							TEXT("fill_hole"),
							Options.bFillHoles))
					{
						return Fail(
							OutError,
							TEXT(
								"modeling_geometry_trim_parameters_invalid"));
					}
					Operation =
						KeepRegion == TEXT("inside")
							? EGeometryScriptBooleanOperation::TrimOutside
							: EGeometryScriptBooleanOperation::TrimInside;
				}
				UGeometryScriptLibrary_MeshBooleanFunctions::
					ApplyMeshBoolean(
						TargetMesh.Get(),
						FTransform::Identity,
						OperandMesh.Get(),
						OperandTransform,
						Operation,
						Options);
			}
			else if (Key == TEXT("voxel_boolean"))
			{
				FString RequestedOperation;
				double VoxelSize = 0.0;
				double Adaptivity = 0.0;
				const FUeAgentCanonicalValuePtr OperandTransformValue =
					FindField(
						Action.Parameters,
						TEXT("operand_to_target"));
				FTransform OperandTransform;
				if (!ReadString(
						Action.Parameters,
						TEXT("operation"),
						RequestedOperation,
						OutError)
					|| !ReadNumber(
						Action.Parameters,
						TEXT("voxel_size_cm"),
						VoxelSize)
					|| !ReadNumber(
						Action.Parameters,
						TEXT("adaptivity"),
						Adaptivity)
					|| !OperandTransformValue.IsValid()
					|| !ReadTransform(
						OperandTransformValue,
						TEXT("translation"),
						TEXT("rotation_quaternion_xyzw"),
						TEXT("scale"),
						OperandTransform))
				{
					return Fail(
						OutError,
						TEXT(
							"modeling_geometry_voxel_boolean_parameters_invalid"));
				}
				UE::Geometry::FVoxelBooleanMeshesOp VoxelBoolean;
				VoxelBoolean.Meshes.Add(
					MakeShared<
						FDynamicMesh3,
						ESPMode::ThreadSafe>(
							Action.WorkingMesh));
				VoxelBoolean.Meshes.Add(
					MakeShared<
						FDynamicMesh3,
						ESPMode::ThreadSafe>(
							Action.OperandMesh));
				VoxelBoolean.Transforms.Add(
					UE::Geometry::FTransformSRT3d::Identity());
				VoxelBoolean.Transforms.Add(
					UE::Geometry::FTransformSRT3d(
						OperandTransform));
				const UE::Geometry::FAxisAlignedBox3d TargetBounds =
					Action.WorkingMesh.GetBounds(true);
				const UE::Geometry::FAxisAlignedBox3d OperandBounds =
					Action.OperandMesh.GetBounds(true);
				const FVector3d TargetSize =
					TargetBounds.Max - TargetBounds.Min;
				const FVector3d OperandSize =
					OperandBounds.Max - OperandBounds.Min;
				const FVector OperandScale =
					OperandTransform.GetScale3D().GetAbs();
				const double MajorAxisSize = FMath::Max(
					FMath::Max3(
						TargetSize.X,
						TargetSize.Y,
						TargetSize.Z),
					FMath::Max3(
						OperandSize.X * OperandScale.X,
						OperandSize.Y * OperandScale.Y,
						OperandSize.Z * OperandScale.Z));
				const double RequestedVoxelCount =
					MajorAxisSize / VoxelSize;
				if (!FMath::IsFinite(RequestedVoxelCount)
					|| RequestedVoxelCount
						> MaximumVoxelBooleanResolutionPerMajorAxis)
				{
					return Fail(
						OutError,
						TEXT(
							"modeling_geometry_voxel_resolution_limit_exceeded"));
				}
				VoxelBoolean.VoxelCount = FMath::Clamp(
					FMath::CeilToInt(RequestedVoxelCount),
					16,
					MaximumVoxelBooleanResolutionPerMajorAxis);
				VoxelBoolean.AdaptivityD = Adaptivity;
				VoxelBoolean.Operation =
					RequestedOperation == TEXT("intersection")
						? UE::Geometry::FVoxelBooleanMeshesOp::
							EBooleanOperation::Intersect
						: RequestedOperation == TEXT("difference")
							? UE::Geometry::FVoxelBooleanMeshesOp::
								EBooleanOperation::DifferenceAB
							: UE::Geometry::FVoxelBooleanMeshesOp::
								EBooleanOperation::Union;
				VoxelBoolean.CalculateResult(nullptr);
				const UE::Geometry::FTransformSRT3d ResultToTarget =
					VoxelBoolean.GetResultTransform();
				TUniquePtr<FDynamicMesh3> Result =
					VoxelBoolean.ExtractResult();
				if (!Result.IsValid()
					|| Result->TriangleCount() == 0)
				{
					return Fail(
						OutError,
						TEXT("modeling_geometry_voxel_boolean_failed"));
				}
				MeshTransforms::ApplyTransform(
					*Result,
					ResultToTarget,
					true);
				Action.WorkingMesh = MoveTemp(*Result);
				return true;
			}
			else if (Key == TEXT("mirror"))
			{
				FVector3d Origin;
				FVector3d Normal;
				bool bKeepOriginal = false;
				bool bWeldSeam = false;
				if (!ReadPlane(Action.Parameters, Origin, Normal)
					|| !ReadBoolean(
						Action.Parameters,
						TEXT("keep_original"),
						bKeepOriginal)
					|| !ReadBoolean(
						Action.Parameters,
						TEXT("weld_seam"),
						bWeldSeam))
				{
					return Fail(
						OutError,
						TEXT("modeling_geometry_mirror_parameters_invalid"));
				}
				FDynamicMesh3 Mirrored = Action.WorkingMesh;
				for (const int32 VertexId
					: Mirrored.VertexIndicesItr())
				{
					const FVector3d Position =
						Mirrored.GetVertex(VertexId);
					Mirrored.SetVertex(
						VertexId,
						Position
							- 2.0
								* FVector3d::DotProduct(
									Position - Origin,
									Normal)
								* Normal);
				}
				Mirrored.ReverseOrientation(true);
				if (bKeepOriginal)
				{
					TStrongObjectPtr<UDynamicMesh> MirroredMesh =
						MakeTransientMesh(Mirrored);
					UGeometryScriptLibrary_MeshBasicEditFunctions::
						AppendMesh(
							TargetMesh.Get(),
							MirroredMesh.Get(),
							FTransform::Identity);
				}
				else
				{
					TargetMesh->SetMesh(MoveTemp(Mirrored));
				}
				if (bWeldSeam)
				{
					FGeometryScriptWeldEdgesOptions WeldOptions;
					WeldOptions.Tolerance = 0.001f;
					UGeometryScriptLibrary_MeshRepairFunctions::
						WeldMeshEdges(
							TargetMesh.Get(),
							WeldOptions);
				}
			}
			else if (Key == TEXT("plane_cut"))
			{
				FVector3d Origin;
				FVector3d Normal;
				FString KeepSide;
				bool bFill = false;
				if (!ReadPlane(Action.Parameters, Origin, Normal)
					|| !ReadString(
						Action.Parameters,
						TEXT("keep_side"),
						KeepSide,
						OutError)
					|| !ReadBoolean(
						Action.Parameters,
						TEXT("fill_hole"),
						bFill))
				{
					return Fail(
						OutError,
						TEXT("modeling_geometry_plane_cut_parameters_invalid"));
				}
				if (KeepSide == TEXT("both"))
				{
					FDynamicMesh3 NegativeHalf =
						Action.WorkingMesh;
					FDynamicMesh3 PositiveHalf =
						Action.WorkingMesh;
					if (!CutMeshHalf(
							NegativeHalf,
							Origin,
							Normal,
							false,
							bFill)
						|| !CutMeshHalf(
							PositiveHalf,
							Origin,
							Normal,
							true,
							bFill))
					{
						return Fail(
							OutError,
							TEXT(
								"modeling_geometry_plane_cut_failed"));
					}
					TargetMesh->SetMesh(MoveTemp(NegativeHalf));
					TStrongObjectPtr<UDynamicMesh> PositiveMesh =
						MakeTransientMesh(PositiveHalf);
					if (!PositiveMesh.IsValid())
					{
						return Fail(
							OutError,
							TEXT(
								"modeling_geometry_transient_mesh_failed"));
					}
					UGeometryScriptLibrary_MeshBasicEditFunctions::
						AppendMesh(
							TargetMesh.Get(),
							PositiveMesh.Get(),
							FTransform::Identity);
				}
				else
				{
					FDynamicMesh3 Half = Action.WorkingMesh;
					if (!CutMeshHalf(
							Half,
							Origin,
							Normal,
							KeepSide == TEXT("positive"),
							bFill))
					{
						return Fail(
							OutError,
							TEXT(
								"modeling_geometry_plane_cut_failed"));
					}
					TargetMesh->SetMesh(MoveTemp(Half));
				}
			}
			else if (Key == TEXT("remesh"))
			{
				double TargetEdgeLength = 0.0;
				int32 Iterations = 0;
				bool bPreserveBoundaries = false;
				bool bPreserveSharpEdges = false;
				if (!ReadNumber(
						Action.Parameters,
						TEXT("target_edge_length_cm"),
						TargetEdgeLength)
					|| !ReadInteger(
						Action.Parameters,
						TEXT("iterations"),
						Iterations)
					|| !ReadBoolean(
						Action.Parameters,
						TEXT("preserve_boundaries"),
						bPreserveBoundaries)
					|| !ReadBoolean(
						Action.Parameters,
						TEXT("preserve_sharp_edges"),
						bPreserveSharpEdges))
				{
					return Fail(
						OutError,
						TEXT("modeling_geometry_remesh_parameters_invalid"));
				}
				bool bRemeshSucceeded = false;
				TargetMesh->EditMesh(
					[&](FDynamicMesh3& EditMesh)
					{
						bRemeshSucceeded = ApplyUniformRemeshWithPolicy(
							EditMesh,
							TargetEdgeLength,
							Iterations,
							bPreserveBoundaries,
							bPreserveSharpEdges);
					},
					EDynamicMeshChangeType::GeneralEdit,
					EDynamicMeshAttributeChangeFlags::Unknown,
					false);
				if (!bRemeshSucceeded)
				{
					return Fail(
						OutError,
						TEXT("modeling_geometry_remesh_failed"));
				}
			}
			else if (Key == TEXT("simplify"))
			{
				FString TargetMode;
				double TargetValue = 0.0;
				bool bPreserveBoundaries = false;
				if (!ReadString(
						Action.Parameters,
						TEXT("target_mode"),
						TargetMode,
						OutError)
					|| !ReadNumber(
						Action.Parameters,
						TEXT("target_value"),
						TargetValue)
					|| !ReadBoolean(
						Action.Parameters,
						TEXT("preserve_boundaries"),
						bPreserveBoundaries))
				{
					return Fail(
						OutError,
						TEXT("modeling_geometry_simplify_parameters_invalid"));
				}
				const bool bUseTolerance =
					TargetMode == TEXT("tolerance_cm");
				if (!bUseTolerance
					&& TargetMode != TEXT("triangle_count")
					&& TargetMode != TEXT("triangle_ratio"))
				{
					return Fail(
						OutError,
						TEXT("modeling_geometry_simplify_target_mode_invalid"),
						TargetMode);
				}
				const int32 TriangleTarget = bUseTolerance
					? 1
					: TargetMode == TEXT("triangle_ratio")
						? FMath::Max(
							1,
							FMath::CeilToInt(
								Action.WorkingMesh.TriangleCount()
								* TargetValue))
						: FMath::Max(
							1,
							FMath::RoundToInt(TargetValue));
				const FBoundarySemanticObservation BeforeBoundary =
					ObserveBoundarySemantics(Action.WorkingMesh);
				TargetMesh->EditMesh(
					[&](FDynamicMesh3& EditMesh)
					{
						ApplySimplifyWithBoundaryPolicy(
							EditMesh,
							TriangleTarget,
							TargetValue,
							bUseTolerance,
							bPreserveBoundaries);
					},
					EDynamicMeshChangeType::GeneralEdit,
					EDynamicMeshAttributeChangeFlags::Unknown,
					false);
				FDynamicMesh3 SimplifiedMesh;
				if (!CopyTransientMesh(
						*TargetMesh,
						SimplifiedMesh,
						OutError))
				{
					return false;
				}
				if (SimplifiedMesh.TriangleCount()
					>= Action.WorkingMesh.TriangleCount())
				{
					return Fail(
						OutError,
						TEXT("modeling_geometry_simplify_no_effect"),
						FString::Printf(
							TEXT("before=%d; after=%d; mode=%s"),
							Action.WorkingMesh.TriangleCount(),
							SimplifiedMesh.TriangleCount(),
							*TargetMode));
				}
				if (!bUseTolerance
					&& SimplifiedMesh.TriangleCount()
						> TriangleTarget + 1)
				{
					return Fail(
						OutError,
						TEXT("modeling_geometry_simplify_target_unreachable"),
						FString::Printf(
							TEXT(
								"target=%d; actual=%d; preserve_boundaries=%s"),
							TriangleTarget,
							SimplifiedMesh.TriangleCount(),
							bPreserveBoundaries
								? TEXT("true")
								: TEXT("false")));
				}
				const FBoundarySemanticObservation AfterBoundary =
					ObserveBoundarySemantics(SimplifiedMesh);
				if (bPreserveBoundaries
					&& AfterBoundary != BeforeBoundary)
				{
					return Fail(
						OutError,
						TEXT(
							"modeling_geometry_simplify_boundary_not_preserved"));
				}
				Action.WorkingMesh = MoveTemp(SimplifiedMesh);
				return true;
			}
			else if (Key == TEXT("subdivide"))
			{
				FString Scheme;
				int32 Levels = 0;
				if (!ReadString(
						Action.Parameters,
						TEXT("scheme"),
						Scheme,
						OutError)
					|| !ReadInteger(
						Action.Parameters,
						TEXT("levels"),
						Levels))
				{
					return Fail(
						OutError,
						TEXT(
							"modeling_geometry_subdivide_parameters_invalid"));
				}
				if (Scheme == TEXT("pn_triangles"))
				{
					UGeometryScriptLibrary_MeshSubdivideFunctions::
						ApplyPNTessellation(
							TargetMesh.Get(),
							FGeometryScriptPNTessellateOptions(),
							Levels);
				}
				else
				{
					UGeometryScriptLibrary_MeshSubdivideFunctions::
						ApplyUniformTessellation(
							TargetMesh.Get(),
							Levels);
				}
			}
			else
			{
				FGeometryScriptMeshSelection Selection;
				if (!BuildSelection(
						*TargetMesh,
						Action.Parameters,
						Selection,
						OutError))
				{
					return false;
				}
				if (Key == TEXT("bevel_edges"))
				{
					double Distance = 0.0;
					double Roundness = 0.0;
					int32 Segments = 0;
					if (!ReadNumber(
							Action.Parameters,
							TEXT("distance_cm"),
							Distance)
						|| !ReadNumber(
							Action.Parameters,
							TEXT("roundness"),
							Roundness)
						|| !ReadInteger(
							Action.Parameters,
							TEXT("segments"),
							Segments))
					{
						return Fail(
							OutError,
							TEXT(
								"modeling_geometry_bevel_parameters_invalid"));
					}
					FGeometryScriptMeshBevelSelectionOptions Options;
					Options.BevelDistance = Distance;
					Options.Subdivisions = Segments;
					Options.RoundWeight = Roundness;
					UGeometryScriptLibrary_MeshModelingFunctions::
						ApplyMeshBevelEdgeSelection(
							TargetMesh.Get(),
							Selection,
							Options);
				}
				else if (Key == TEXT("duplicate_faces"))
				{
					TArray<double> Translation;
					if (!ReadNumberArray(
							Action.Parameters,
							TEXT("translation_cm"),
							3,
							Translation))
					{
						return Fail(
							OutError,
							TEXT(
								"modeling_geometry_duplicate_parameters_invalid"));
					}
					FGeometryScriptMeshSelection NewTriangles;
					UGeometryScriptLibrary_MeshModelingFunctions::
						ApplyMeshDuplicateFaces(
							TargetMesh.Get(),
							Selection,
							NewTriangles);
					UGeometryScriptLibrary_MeshTransformFunctions::
						TranslateMeshSelection(
							TargetMesh.Get(),
							NewTriangles,
							FVector(
								Translation[0],
								Translation[1],
								Translation[2]));
				}
				else if (Key == TEXT("extrude_faces")
					|| Key == TEXT("push_pull_faces"))
				{
					double Distance = 0.0;
					FString DirectionMode;
					if (!ReadNumber(
							Action.Parameters,
							TEXT("distance_cm"),
							Distance)
						|| !ReadString(
							Action.Parameters,
							TEXT("direction_mode"),
							DirectionMode,
							OutError))
					{
						return Fail(
							OutError,
							TEXT(
								"modeling_geometry_extrude_parameters_invalid"));
					}
					if (Key == TEXT("extrude_faces")
						&& DirectionMode == TEXT("vertex_normals"))
					{
						bool bExtruded = false;
						TargetMesh->EditMesh(
							[&](FDynamicMesh3& EditMesh)
							{
								EnsureTopologyVertexNormals(EditMesh);
								TArray<int32> Triangles;
								Selection.ConvertToMeshIndexArray(
									EditMesh,
									Triangles,
									EGeometryScriptIndexType::Triangle);
								UE::Geometry::FOffsetMeshRegion Extruder(
									&EditMesh);
								Extruder.Triangles = MoveTemp(Triangles);
								Extruder.ExtrusionVectorType =
									UE::Geometry::FOffsetMeshRegion::
										EVertexExtrusionVectorType::VertexNormal;
								Extruder.DefaultOffsetDistance = Distance;
								Extruder.bIsPositiveOffset = Distance > 0.0;
								bExtruded = Extruder.Apply();
							},
							EDynamicMeshChangeType::GeneralEdit,
							EDynamicMeshAttributeChangeFlags::Unknown,
							false);
						if (!bExtruded)
						{
							return Fail(
								OutError,
								TEXT("modeling_geometry_extrude_failed"));
						}
					}
					else
					{
						FGeometryScriptMeshLinearExtrudeOptions Options;
						Options.Distance = Distance;
						Options.DirectionMode =
							DirectionMode == TEXT("fixed")
								? EGeometryScriptLinearExtrudeDirection::
									FixedDirection
								: EGeometryScriptLinearExtrudeDirection::
									AverageFaceNormal;
						if (DirectionMode == TEXT("fixed"))
						{
							FVector3d Direction;
							if (!ReadDirection(
									Action.Parameters,
									Direction))
							{
								return Fail(
									OutError,
									TEXT(
										"modeling_geometry_direction_invalid"));
							}
							Options.Direction = FVector(Direction);
						}
						UGeometryScriptLibrary_MeshModelingFunctions::
							ApplyMeshLinearExtrudeFaces(
								TargetMesh.Get(),
								Options,
								Selection);
					}
				}
				else if (Key == TEXT("inset_faces"))
				{
					double Distance = 0.0;
					FString BoundaryMode;
					if (!ReadNumber(
							Action.Parameters,
							TEXT("distance_cm"),
							Distance)
						|| !ReadString(
							Action.Parameters,
							TEXT("boundary_mode"),
							BoundaryMode,
							OutError))
					{
						return Fail(
							OutError,
							TEXT(
								"modeling_geometry_inset_parameters_invalid"));
					}
					FGeometryScriptMeshInsetOutsetFacesOptions Options;
					Options.Distance = Distance;
					Options.bReproject =
						BoundaryMode == TEXT("reprojected");
					UGeometryScriptLibrary_MeshModelingFunctions::
						ApplyMeshInsetOutsetFaces(
							TargetMesh.Get(),
							Options,
							Selection);
				}
				else if (Key == TEXT("offset_faces"))
				{
					double Distance = 0.0;
					FString OffsetMode;
					if (!ReadNumber(
							Action.Parameters,
							TEXT("distance_cm"),
							Distance)
						|| !ReadString(
							Action.Parameters,
							TEXT("offset_mode"),
							OffsetMode,
							OutError))
					{
						return Fail(
							OutError,
							TEXT(
								"modeling_geometry_offset_parameters_invalid"));
					}
					FGeometryScriptMeshOffsetFacesOptions Options;
					Options.Distance = Distance;
					Options.OffsetType =
						OffsetMode == TEXT("face_normals")
							? EGeometryScriptOffsetFacesType::FaceNormal
							: EGeometryScriptOffsetFacesType::VertexNormal;
					if (OffsetMode == TEXT("vertex_normals"))
					{
						TargetMesh->EditMesh(
							[](FDynamicMesh3& EditMesh)
							{
								EnsureTopologyVertexNormals(EditMesh);
							},
							EDynamicMeshChangeType::AttributeEdit,
							EDynamicMeshAttributeChangeFlags::NormalsTangents,
							false);
					}
					UGeometryScriptLibrary_MeshModelingFunctions::
						ApplyMeshOffsetFaces(
							TargetMesh.Get(),
							Options,
							Selection);
				}
				else
				{
					return Fail(
						OutError,
						TEXT(
							"modeling_geometry_topology_operation_unsupported"),
						Key);
				}
			}

			return CopyTransientMesh(
				*TargetMesh,
				Action.WorkingMesh,
				OutError);
		}

		bool NormalizeUvLayer(
			FDynamicMesh3& Mesh,
			const int32 Channel)
		{
			if (!Mesh.HasAttributes()
				|| Channel < 0
				|| Channel >= Mesh.Attributes()->NumUVLayers())
			{
				return false;
			}
			FDynamicMeshUVOverlay* Overlay =
				Mesh.Attributes()->GetUVLayer(Channel);
			if (!Overlay || Overlay->ElementCount() == 0)
			{
				return false;
			}
			FVector2f Minimum(
				TNumericLimits<float>::Max(),
				TNumericLimits<float>::Max());
			FVector2f Maximum(
				-TNumericLimits<float>::Max(),
				-TNumericLimits<float>::Max());
			for (const int32 ElementId
				: Overlay->ElementIndicesItr())
			{
				const FVector2f Value =
					Overlay->GetElement(ElementId);
				Minimum.X = FMath::Min(Minimum.X, Value.X);
				Minimum.Y = FMath::Min(Minimum.Y, Value.Y);
				Maximum.X = FMath::Max(Maximum.X, Value.X);
				Maximum.Y = FMath::Max(Maximum.Y, Value.Y);
			}
			if (Minimum.X >= 0.0f
				&& Minimum.Y >= 0.0f
				&& Maximum.X <= 1.0f
				&& Maximum.Y <= 1.0f)
			{
				return true;
			}
			const float Range = FMath::Max(
				Maximum.X - Minimum.X,
				Maximum.Y - Minimum.Y);
			if (Range <= UE_SMALL_NUMBER)
			{
				return false;
			}
			for (const int32 ElementId
				: Overlay->ElementIndicesItr())
			{
				Overlay->SetElement(
					ElementId,
					(Overlay->GetElement(ElementId) - Minimum)
						/ Range);
			}
			return true;
		}

		bool ConvertStaticMesh(
			UStaticMesh& StaticMesh,
			FDynamicMesh3& OutMesh)
		{
			const FMeshDescription* Description =
				StaticMesh.GetMeshDescription(0);
			if (!Description)
			{
				return false;
			}
			FMeshDescriptionToDynamicMesh Converter;
			Converter.Convert(Description, OutMesh);
			return true;
		}

		bool TryCopyResolvedCarrierMesh(
			UObject& Object,
			UWorld& World,
			FDynamicMesh3& OutMesh)
		{
			if (ADynamicMeshActor* Actor = Cast<ADynamicMeshActor>(&Object))
			{
				UDynamicMesh* DynamicMesh =
					Actor->GetDynamicMeshComponent()
						? Actor->GetDynamicMeshComponent()->GetDynamicMesh()
						: nullptr;
				if (Actor->GetWorld() != &World || !DynamicMesh)
				{
					return false;
				}
				bool bCopied = false;
				DynamicMesh->ProcessMesh(
					[&OutMesh, &bCopied](const FDynamicMesh3& Mesh)
					{
						OutMesh = Mesh;
						bCopied = true;
					});
				return bCopied
					&& OutMesh.VertexCount() > 0
					&& OutMesh.TriangleCount() > 0;
			}
			if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(&Object))
			{
				return ConvertStaticMesh(*StaticMesh, OutMesh)
					&& OutMesh.VertexCount() > 0
					&& OutMesh.TriangleCount() > 0;
			}
			return false;
		}

		bool CommitStaticMesh(
			UStaticMesh& StaticMesh,
			const FDynamicMesh3& Mesh)
		{
			FMeshDescription Description;
			FStaticMeshAttributes Attributes(Description);
			Attributes.Register();
			FDynamicMeshToMeshDescription Converter;
			Converter.Convert(&Mesh, Description);
			StaticMesh.CreateMeshDescription(0, Description);
			StaticMesh.CommitMeshDescription(0);
			StaticMesh.PostEditChange();
			return StaticMesh.GetMeshDescription(0) != nullptr;
		}

		bool CheckoutExistingCarrier(
			UWorld& World,
			IUeAgentModelingGeometryEffectReferenceResolver& Resolver,
			const FPlannedChange& Change,
			FModelingGeometryAction& OutAction,
			FExecutionMutationError& OutError)
		{
			const FUeAgentCanonicalValuePtr& After =
				Change.GetAfter();
			FString CarrierKind;
			FString ExpectedFingerprint;
			if (Change.GetIntent() != EChangeIntent::Update
				|| !ReadString(
					After,
					TEXT("operation_key"),
					OutAction.OperationKey,
					OutError)
				|| !ReadString(
					After,
					TEXT("target_ref"),
					OutAction.TargetRef,
					OutError)
				|| !ReadString(
					After,
					TEXT("carrier_kind"),
					CarrierKind,
					OutError)
				|| !ReadString(
					After,
					TEXT("expected_before_fingerprint"),
					ExpectedFingerprint,
					OutError))
			{
				return false;
			}
			OutAction.Parameters =
				FindField(After, TEXT("typed_parameters"));
			if (!OutAction.Parameters.IsValid())
			{
				return Fail(
					OutError,
					TEXT("modeling_geometry_parameters_missing"));
			}
			UObject* Object = nullptr;
			if (!Resolver.TryResolveReference(
					World,
					OutAction.TargetRef,
					Object,
					OutError)
				|| !Object)
			{
				return false;
			}
			if (CarrierKind == TEXT("dynamic_mesh_actor"))
			{
				ADynamicMeshActor* Actor =
					Cast<ADynamicMeshActor>(Object);
				UDynamicMeshComponent* Component =
					Actor
						? Actor->GetDynamicMeshComponent()
						: nullptr;
				UDynamicMesh* DynamicMesh =
					Component
						? Component->GetDynamicMesh()
						: nullptr;
				if (!Actor
					|| Actor->GetWorld() != &World
					|| !Component
					|| !DynamicMesh)
				{
					return Fail(
						OutError,
						TEXT(
							"modeling_geometry_dynamic_carrier_invalid"));
				}
				DynamicMesh->ProcessMesh(
					[&OutAction](const FDynamicMesh3& Mesh)
					{
						OutAction.OriginalMesh = Mesh;
						OutAction.WorkingMesh = Mesh;
					});
				OutAction.Carrier = ECarrierKind::DynamicMeshActor;
				OutAction.DynamicActor = Actor;
				OutAction.Package = Actor->GetOutermost();
				for (int32 Index = 0;
					Index < Component->GetNumMaterials();
					++Index)
				{
					OutAction.OriginalDynamicMaterials.Add(
						Component->GetMaterial(Index));
				}
				OutAction.OriginalCollision =
					Component->GetSimpleCollisionShapes();
				OutAction.bHadCollision =
					OutAction.OriginalCollision.GetElementCount() > 0;
			}
			else if (CarrierKind == TEXT("static_mesh_asset"))
			{
				UStaticMesh* StaticMesh = Cast<UStaticMesh>(Object);
				if (!StaticMesh
					|| !ConvertStaticMesh(
						*StaticMesh,
						OutAction.OriginalMesh))
				{
					return Fail(
						OutError,
						TEXT(
							"modeling_geometry_static_carrier_invalid"));
				}
				OutAction.WorkingMesh = OutAction.OriginalMesh;
				OutAction.Carrier = ECarrierKind::StaticMeshAsset;
				OutAction.StaticMesh = StaticMesh;
				OutAction.Package = StaticMesh->GetOutermost();
				OutAction.OriginalStaticMaterials =
					StaticMesh->GetStaticMaterials();
				if (const UBodySetup* BodySetup =
					StaticMesh->GetBodySetup())
				{
					OutAction.OriginalCollision =
						BodySetup->AggGeom;
					OutAction.bHadCollision = true;
				}
			}
			else
			{
				return Fail(
					OutError,
					TEXT("modeling_geometry_carrier_unsupported"));
			}
			if (MeshFingerprint(OutAction.OriginalMesh)
				!= ExpectedFingerprint)
			{
				return Fail(
					OutError,
					TEXT(
						"modeling_geometry_checkout_fingerprint_mismatch"));
			}
			OutAction.bPackageWasDirty =
				OutAction.Package.IsValid()
				&& OutAction.Package->IsDirty();
			OutAction.CanonicalKey =
				Change.GetIdentity().GetCanonicalKey();

			const FUeAgentCanonicalValuePtr OperandRefValue =
				FindField(After, TEXT("operand_ref"));
			if (OperandRefValue.IsValid())
			{
				OutAction.OperandRef =
					OperandRefValue->GetText();
				UObject* OperandObject = nullptr;
				if (!Resolver.TryResolveReference(
						World,
						OutAction.OperandRef,
						OperandObject,
						OutError))
				{
					return false;
				}
				if (!OperandObject
					|| !TryCopyResolvedCarrierMesh(
						*OperandObject,
						World,
						OutAction.OperandMesh))
				{
					return Fail(
						OutError,
						TEXT(
							"modeling_geometry_operand_carrier_invalid"));
				}
				FString ExpectedOperandFingerprint;
				if (!ReadString(
						After,
						TEXT("operand_before_fingerprint"),
						ExpectedOperandFingerprint,
						OutError)
					|| MeshFingerprint(OutAction.OperandMesh)
						!= ExpectedOperandFingerprint)
				{
					return Fail(
						OutError,
						TEXT(
							"modeling_geometry_operand_fingerprint_mismatch"));
				}
			}
			return true;
		}

		bool PreparePrimitive(
			const FPlannedChange& Change,
			FModelingGeometryAction& OutAction,
			FExecutionMutationError& OutError)
		{
			const FUeAgentCanonicalValuePtr& After =
				Change.GetAfter();
			if (Change.GetIntent() != EChangeIntent::Create
				|| !ReadString(
					After,
					TEXT("operation_key"),
					OutAction.OperationKey,
					OutError)
				|| !ReadString(
					After,
					TEXT("target_ref"),
					OutAction.TargetRef,
					OutError))
			{
				return false;
			}
			OutAction.Parameters =
				FindField(After, TEXT("typed_parameters"));
			const FUeAgentCanonicalValuePtr Placement =
				FindField(OutAction.Parameters, TEXT("placement"));
			TArray<double> Translation;
			TArray<double> Rotation;
			TArray<double> Scale;
			FString Unit;
			if (!Placement.IsValid()
				|| !ReadString(
					Placement,
					TEXT("unit"),
					Unit,
					OutError)
				|| !ReadNumberArray(
					Placement,
					TEXT("translation"),
					3,
					Translation)
				|| !ReadNumberArray(
					Placement,
					TEXT("rotation_quaternion_xyzw"),
					4,
					Rotation)
				|| !ReadNumberArray(
					Placement,
					TEXT("scale"),
					3,
					Scale))
			{
				return Fail(
					OutError,
					TEXT("modeling_geometry_placement_invalid"));
			}
			const double UnitScale =
				Unit == TEXT("meter") ? 100.0 : 1.0;
			FQuat Quaternion(
				Rotation[0],
				Rotation[1],
				Rotation[2],
				Rotation[3]);
			if (Quaternion.SizeSquared() <= UE_DOUBLE_SMALL_NUMBER)
			{
				return Fail(
					OutError,
					TEXT("modeling_geometry_placement_invalid"));
			}
			Quaternion.Normalize();
			OutAction.Placement = FTransform(
				Quaternion,
				FVector(
					Translation[0] * UnitScale,
					Translation[1] * UnitScale,
					Translation[2] * UnitScale),
				FVector(Scale[0], Scale[1], Scale[2]));
			OutAction.Carrier = ECarrierKind::PrimitiveOutput;
			OutAction.CanonicalKey =
				Change.GetIdentity().GetCanonicalKey();
			return OutAction.Parameters.IsValid();
		}

		bool ApplyAttributeOperation(
			FModelingGeometryAction& Action,
			IUeAgentModelingGeometryEffectReferenceResolver& Resolver,
			UWorld& World,
			FExecutionMutationError& OutError)
		{
			if (Action.OperationKey == TEXT("auto_uv")
				|| Action.OperationKey == TEXT("project_uv"))
			{
				int32 Channel = 0;
				if (!ReadInteger(
						Action.Parameters,
						TEXT("uv_channel"),
						Channel))
				{
					return Fail(
						OutError,
						TEXT("modeling_geometry_uv_channel_invalid"));
				}
				TStrongObjectPtr<UDynamicMesh> DynamicMesh =
					MakeTransientMesh(Action.WorkingMesh);
				if (!DynamicMesh.IsValid())
				{
					return Fail(
						OutError,
						TEXT("modeling_geometry_transient_mesh_failed"));
				}
				int32 ExistingUvLayers = 0;
				DynamicMesh->ProcessMesh(
					[&ExistingUvLayers](const FDynamicMesh3& Mesh)
					{
						ExistingUvLayers = Mesh.HasAttributes()
							? Mesh.Attributes()->NumUVLayers()
							: 0;
					});
				if (ExistingUvLayers <= Channel)
				{
					UGeometryScriptLibrary_MeshUVFunctions::
						SetNumUVSets(
							DynamicMesh.Get(),
							Channel + 1);
				}
				if (Action.OperationKey == TEXT("auto_uv"))
				{
					FString Method;
					FString IslandSource;
					bool bPack = false;
					int32 TextureResolution = 0;
					if (!ReadString(
							Action.Parameters,
							TEXT("method"),
							Method,
							OutError)
						|| !ReadString(
							Action.Parameters,
							TEXT("island_source"),
							IslandSource,
							OutError)
						|| !ReadBoolean(
							Action.Parameters,
							TEXT("pack"),
							bPack)
						|| !ReadInteger(
							Action.Parameters,
							TEXT("texture_resolution"),
							TextureResolution))
					{
						return Fail(
							OutError,
							TEXT(
								"modeling_geometry_auto_uv_parameters_invalid"));
					}
					if (Method == TEXT("xatlas"))
					{
						UGeometryScriptLibrary_MeshUVFunctions::
							AutoGenerateXAtlasMeshUVs(
								DynamicMesh.Get(),
								Channel,
								FGeometryScriptXAtlasOptions());
					}
					else
					{
						FGeometryScriptPatchBuilderOptions Options;
						Options.bRespectInputGroups =
							IslandSource == TEXT("polygroups");
						Options.bAutoPack = bPack;
						Options.PackingOptions.TargetImageWidth =
							TextureResolution;
						UGeometryScriptLibrary_MeshUVFunctions::
							AutoGeneratePatchBuilderMeshUVs(
								DynamicMesh.Get(),
								Channel,
								Options);
					}
					if (bPack && Method == TEXT("xatlas"))
					{
						FGeometryScriptRepackUVsOptions Options;
						Options.TargetImageWidth = TextureResolution;
						UGeometryScriptLibrary_MeshUVFunctions::
							RepackMeshUVs(
								DynamicMesh.Get(),
								Channel,
								Options);
					}
				}
				else
				{
					FString ProjectionType;
					TArray<double> Dimensions;
					const FUeAgentCanonicalValuePtr Frame =
						FindField(
							Action.Parameters,
							TEXT("projection_frame"));
					TArray<double> Origin;
					TArray<double> AxisU;
					TArray<double> AxisV;
					TArray<double> AxisW;
					if (!ReadString(
							Action.Parameters,
							TEXT("projection_type"),
							ProjectionType,
							OutError)
						|| !ReadNumberArray(
							Action.Parameters,
							TEXT("dimensions_cm"),
							3,
							Dimensions)
						|| !Frame.IsValid()
						|| !ReadNumberArray(
							Frame,
							TEXT("origin"),
							3,
							Origin)
						|| !ReadNumberArray(
							Frame,
							TEXT("axis_u"),
							3,
							AxisU)
						|| !ReadNumberArray(
							Frame,
							TEXT("axis_v"),
							3,
							AxisV)
						|| !ReadNumberArray(
							Frame,
							TEXT("axis_w"),
							3,
							AxisW))
					{
						return Fail(
							OutError,
							TEXT(
								"modeling_geometry_project_uv_parameters_invalid"));
					}
					const FVector U(
						AxisU[0],
						AxisU[1],
						AxisU[2]);
					const FVector V(
						AxisV[0],
						AxisV[1],
						AxisV[2]);
					const FVector W(
						AxisW[0],
						AxisW[1],
						AxisW[2]);
					if (!U.IsNormalized()
						|| !V.IsNormalized()
						|| !W.IsNormalized()
						|| FMath::Abs(FVector::DotProduct(U, V))
							> 0.001f
						|| FVector::DotProduct(
							FVector::CrossProduct(U, V),
							W)
							< 0.999f)
					{
						return Fail(
							OutError,
							TEXT(
								"modeling_geometry_projection_basis_invalid"));
					}
					const FTransform ProjectionTransform(
						FRotationMatrix::MakeFromXY(U, V).ToQuat(),
						FVector(Origin[0], Origin[1], Origin[2]),
						FVector(
							Dimensions[0],
							Dimensions[1],
							Dimensions[2]));
					const FGeometryScriptMeshSelection AllTriangles;
					if (ProjectionType == TEXT("box"))
					{
						UGeometryScriptLibrary_MeshUVFunctions::
							SetMeshUVsFromBoxProjection(
								DynamicMesh.Get(),
								Channel,
								ProjectionTransform,
								AllTriangles);
					}
					else if (ProjectionType == TEXT("cylindrical"))
					{
						UGeometryScriptLibrary_MeshUVFunctions::
							SetMeshUVsFromCylinderProjection(
								DynamicMesh.Get(),
								Channel,
								ProjectionTransform,
								AllTriangles);
					}
					else
					{
						UGeometryScriptLibrary_MeshUVFunctions::
							SetMeshUVsFromPlanarProjection(
								DynamicMesh.Get(),
								Channel,
								ProjectionTransform,
								AllTriangles);
					}
				}
				if (!CopyTransientMesh(
						*DynamicMesh,
						Action.WorkingMesh,
						OutError)
					|| !NormalizeUvLayer(
						Action.WorkingMesh,
						Channel))
				{
					return Fail(
						OutError,
						TEXT("modeling_geometry_uv_generation_failed"));
				}
				return true;
			}
			if (Action.OperationKey == TEXT("recompute_normals"))
			{
				FString Mode;
				bool bPreserveHardEdges = false;
				if (!ReadString(
						Action.Parameters,
						TEXT("mode"),
						Mode,
						OutError)
					|| !ReadBoolean(
						Action.Parameters,
						TEXT("preserve_existing_hard_edges"),
						bPreserveHardEdges))
				{
					return Fail(
						OutError,
						TEXT(
							"modeling_geometry_normals_parameters_invalid"));
				}
				TStrongObjectPtr<UDynamicMesh> DynamicMesh =
					MakeTransientMesh(Action.WorkingMesh);
				if (!DynamicMesh.IsValid())
				{
					return Fail(
						OutError,
						TEXT("modeling_geometry_transient_mesh_failed"));
				}
				if (Mode == TEXT("face"))
				{
					UGeometryScriptLibrary_MeshNormalsFunctions::
						SetPerFaceNormals(DynamicMesh.Get());
				}
				else if (Mode == TEXT("angle_threshold"))
				{
					double Angle = 30.0;
					ReadNumber(
						Action.Parameters,
						TEXT("angle_degrees"),
						Angle);
					FGeometryScriptSplitNormalsOptions SplitOptions;
					SplitOptions.OpeningAngleDeg = Angle;
					UGeometryScriptLibrary_MeshNormalsFunctions::
						ComputeSplitNormals(
							DynamicMesh.Get(),
							SplitOptions,
							FGeometryScriptCalculateNormalsOptions());
				}
				else if (bPreserveHardEdges)
				{
					UGeometryScriptLibrary_MeshNormalsFunctions::
						RecomputeNormals(
							DynamicMesh.Get(),
							FGeometryScriptCalculateNormalsOptions());
				}
				else
				{
					UGeometryScriptLibrary_MeshNormalsFunctions::
						SetPerVertexNormals(DynamicMesh.Get());
				}
				return CopyTransientMesh(
					*DynamicMesh,
					Action.WorkingMesh,
					OutError);
			}
			if (Action.OperationKey == TEXT("assign_material_slot"))
			{
				int32 SlotIndex = 0;
				const FUeAgentCanonicalValuePtr Selection =
					FindField(Action.Parameters, TEXT("selection"));
				TArray<int32> TriangleIds;
				if (!ReadInteger(
						Action.Parameters,
						TEXT("slot_index"),
						SlotIndex)
					|| !ReadElementIds(
						Selection,
						TEXT("element_ids"),
						TriangleIds))
				{
					return Fail(
						OutError,
						TEXT(
							"modeling_geometry_material_assignment_invalid"));
				}
				Action.WorkingMesh.EnableAttributes();
				FDynamicMeshAttributeSet* Attributes =
					Action.WorkingMesh.Attributes();
				Attributes->EnableMaterialID();
				FDynamicMeshMaterialAttribute* MaterialIds =
					Attributes->GetMaterialID();
				for (const int32 TriangleId : TriangleIds)
				{
					if (!Action.WorkingMesh.IsTriangle(TriangleId))
					{
						return Fail(
							OutError,
							TEXT(
								"modeling_geometry_material_selection_stale"));
					}
					MaterialIds->SetValue(TriangleId, SlotIndex);
				}
				return true;
			}
			if (Action.OperationKey == TEXT("add_material_slot"))
			{
				FString MaterialRef;
				FString SlotName;
				if (!ReadString(
						Action.Parameters,
						TEXT("material_ref"),
						MaterialRef,
						OutError)
					|| !ReadString(
						Action.Parameters,
						TEXT("slot_name"),
						SlotName,
						OutError))
				{
					return false;
				}
				UObject* MaterialObject = nullptr;
				if (!Resolver.TryResolveReference(
						World,
						MaterialRef,
						MaterialObject,
						OutError))
				{
					return false;
				}
				UMaterialInterface* Material =
					Cast<UMaterialInterface>(MaterialObject);
				if (!Material)
				{
					return Fail(
						OutError,
						TEXT(
							"modeling_geometry_material_reference_invalid"));
				}
				if (Action.Carrier
					!= ECarrierKind::StaticMeshAsset)
				{
					return Fail(
						OutError,
						TEXT(
							"modeling_geometry_named_slot_carrier_unsupported"));
				}
				Action.StaticMesh->GetStaticMaterials().Add(
					FStaticMaterial(
						Material,
						FName(*SlotName),
						FName(*SlotName)));
				return true;
			}
			return Fail(
				OutError,
				TEXT(
					"modeling_geometry_attribute_operation_unsupported"),
				Action.OperationKey);
		}

		bool ApplyCollisionOperation(
			FModelingGeometryAction& Action,
			FExecutionMutationError& OutError)
		{
			bool bReplace = true;
			if (!ReadBoolean(
					Action.Parameters,
					TEXT("replace_existing"),
					bReplace))
			{
				return Fail(
					OutError,
					TEXT(
						"modeling_geometry_collision_parameters_invalid"));
			}
			TStrongObjectPtr<UDynamicMesh> SourceMesh =
				MakeTransientMesh(Action.WorkingMesh);
			if (!SourceMesh.IsValid())
			{
				return Fail(
					OutError,
					TEXT("modeling_geometry_transient_mesh_failed"));
			}
			FGeometryScriptCollisionFromMeshOptions Options;
			Options.bEmitTransaction = false;
			if (Action.OperationKey
				== TEXT("generate_convex_collision"))
			{
				FString Mode;
				int32 MaxHulls = 0;
				int32 MaxVertices = 0;
				if (!ReadString(
						Action.Parameters,
						TEXT("mode"),
						Mode,
						OutError)
					|| !ReadInteger(
						Action.Parameters,
						TEXT("max_hulls"),
						MaxHulls)
					|| !ReadInteger(
						Action.Parameters,
						TEXT("max_vertices_per_hull"),
						MaxVertices))
				{
					return Fail(
						OutError,
						TEXT(
							"modeling_geometry_convex_collision_parameters_invalid"));
				}
				Options.Method =
					EGeometryScriptCollisionGenerationMethod::
						ConvexHulls;
				Options.bAutoDetectBoxes = false;
				Options.bAutoDetectCapsules = false;
				Options.bAutoDetectSpheres = false;
				Options.bSimplifyHulls = true;
				Options.MaxConvexHullsPerMesh =
					Mode == TEXT("single_hull") ? 1 : MaxHulls;
				Options.MaxShapeCount =
					Mode == TEXT("single_hull") ? 1 : MaxHulls;
				Options.ConvexHullTargetFaceCount =
					FMath::Max(4, MaxVertices * 2 - 4);
			}
			else if (Action.OperationKey
				== TEXT("generate_simple_collision"))
			{
				FString Method;
				int32 MaxShapes = 0;
				if (!ReadString(
						Action.Parameters,
						TEXT("method"),
						Method,
						OutError)
					|| !ReadInteger(
						Action.Parameters,
						TEXT("max_shapes"),
						MaxShapes))
				{
					return Fail(
						OutError,
						TEXT(
							"modeling_geometry_simple_collision_parameters_invalid"));
				}
				Options.Method =
					Method == TEXT("capsules")
						? EGeometryScriptCollisionGenerationMethod::Capsules
						: Method == TEXT("spheres")
							? EGeometryScriptCollisionGenerationMethod::
								MinimalSpheres
							: Method == TEXT("min_volume_shapes")
								? EGeometryScriptCollisionGenerationMethod::
									MinVolumeShapes
								: EGeometryScriptCollisionGenerationMethod::
									AlignedBoxes;
				Options.MaxShapeCount = MaxShapes;
				Options.bAutoDetectBoxes =
					Method == TEXT("min_volume_shapes");
				Options.bAutoDetectCapsules =
					Method == TEXT("min_volume_shapes");
				Options.bAutoDetectSpheres =
					Method == TEXT("min_volume_shapes");
			}
			else
			{
				return Fail(
					OutError,
					TEXT(
						"modeling_geometry_collision_operation_unsupported"),
					Action.OperationKey);
			}
			FGeometryScriptSimpleCollision Generated =
				UGeometryScriptLibrary_CollisionFunctions::
					GenerateCollisionFromMesh(
						SourceMesh.Get(),
						Options);
			if (Generated.AggGeom.GetElementCount() == 0)
			{
				return Fail(
					OutError,
					TEXT("modeling_geometry_collision_generation_failed"));
			}
			FGeometryScriptSimpleCollision FinalCollision;
			if (!bReplace)
			{
				FinalCollision.AggGeom = Action.OriginalCollision;
				UGeometryScriptLibrary_CollisionFunctions::
					CombineSimpleCollision(
						FinalCollision,
						Generated);
			}
			else
			{
				FinalCollision = MoveTemp(Generated);
			}
			if (Action.Carrier == ECarrierKind::DynamicMeshActor)
			{
				UDynamicMeshComponent* Component =
					Action.DynamicActor->GetDynamicMeshComponent();
				Component->SetSimpleCollisionShapes(
					FinalCollision.AggGeom,
					false);
				Component->GetBodySetup()->AggGeom =
					FinalCollision.AggGeom;
			}
			else
			{
				UStaticMesh* StaticMesh = Action.StaticMesh.Get();
				if (!StaticMesh->GetBodySetup())
				{
					StaticMesh->CreateBodySetup();
				}
				StaticMesh->GetBodySetup()->AggGeom =
					FinalCollision.AggGeom;
				StaticMesh->GetBodySetup()->InvalidatePhysicsData();
				StaticMesh->GetBodySetup()->CreatePhysicsMeshes();
			}
			return true;
		}

		bool CommitAction(
			FModelingGeometryAction& Action,
			IUeAgentModelingGeometryEffectReferenceResolver& Resolver,
			UWorld& World,
			FExecutionMutationError& OutError)
		{
			if (Action.Carrier == ECarrierKind::PrimitiveOutput)
			{
				if (!GeneratePrimitive(
						Action.OperationKey,
						Action.Parameters,
						Action.WorkingMesh,
						OutError))
				{
					return false;
				}
				Action.Package = World.GetOutermost();
				Action.bPackageWasDirty =
					Action.Package.IsValid()
						&& Action.Package->IsDirty();
				FActorSpawnParameters SpawnParameters;
				SpawnParameters.SpawnCollisionHandlingOverride =
					ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
				SpawnParameters.ObjectFlags |= RF_Transactional;
				ADynamicMeshActor* Actor =
					World.SpawnActor<ADynamicMeshActor>(
						Action.Placement.GetLocation(),
						Action.Placement.Rotator(),
						SpawnParameters);
				if (!Actor)
				{
					return Fail(
						OutError,
						TEXT(
							"modeling_geometry_primitive_spawn_failed"));
				}
				Actor->SetActorScale3D(
					Action.Placement.GetScale3D());
				Actor->SetActorLabel(Action.TargetRef);
				// Primitive outputs are world actors rather than asset-registry
				// objects. Give each exact target its own actor folder so the
				// public destructive scope command can clean it up using the
				// unchanged target identity.
				Actor->SetFolderPath(FName(*Action.TargetRef));
				UDynamicMeshComponent* Component =
					Actor->GetDynamicMeshComponent();
				UDynamicMesh* DynamicMesh =
					Component ? Component->GetDynamicMesh() : nullptr;
				if (!DynamicMesh)
				{
					World.DestroyActor(Actor);
					return Fail(
						OutError,
						TEXT(
							"modeling_geometry_primitive_mesh_missing"));
				}
				DynamicMesh->EditMesh(
					[&Action](FDynamicMesh3& Mesh)
					{
						Mesh = Action.WorkingMesh;
					});
				Action.CreatedActor = Actor;
				return true;
			}

			const FString& TargetKind =
				Action.OperationKey.StartsWith(TEXT("generate_"))
					? TEXT("collision")
					: Action.OperationKey == TEXT("add_material_slot")
						|| Action.OperationKey == TEXT("auto_uv")
						|| Action.OperationKey == TEXT("project_uv")
						|| Action.OperationKey
							== TEXT("recompute_normals")
						|| Action.OperationKey
							== TEXT("assign_material_slot")
						? TEXT("attribute")
						: TEXT("topology");
			bool bApplied = false;
			if (TargetKind == TEXT("topology"))
			{
				bApplied = ApplyTopologyOperation(
					Action,
					OutError);
			}
			else if (TargetKind == TEXT("attribute"))
			{
				bApplied = ApplyAttributeOperation(
					Action,
					Resolver,
					World,
					OutError);
			}
			else
			{
				bApplied = ApplyCollisionOperation(
					Action,
					OutError);
			}
			if (!bApplied)
			{
				return false;
			}

			if (TargetKind != TEXT("collision")
				&& Action.OperationKey
					!= TEXT("add_material_slot"))
			{
				if (Action.Carrier
					== ECarrierKind::DynamicMeshActor)
				{
					UDynamicMeshComponent* Component =
						Action.DynamicActor->GetDynamicMeshComponent();
					Component->Modify();
					Component->GetDynamicMesh()->EditMesh(
						[&Action](FDynamicMesh3& Mesh)
						{
							Mesh = Action.WorkingMesh;
						});
					Component->NotifyMeshUpdated();
				}
				else if (!CommitStaticMesh(
					*Action.StaticMesh,
					Action.WorkingMesh))
				{
					return Fail(
						OutError,
						TEXT(
							"modeling_geometry_static_commit_failed"));
				}
			}
			if (Action.Package.IsValid())
			{
				Action.Package->SetDirtyFlag(true);
			}
			return true;
		}

		bool RollbackAction(
			UWorld& World,
			FModelingGeometryAction& Action)
		{
			if (Action.Carrier == ECarrierKind::PrimitiveOutput)
			{
				ADynamicMeshActor* Actor = Action.CreatedActor.Get();
				const bool bDestroyed = !Actor
					|| (Actor->GetWorld() == &World
						&& World.DestroyActor(Actor));
				if (Action.Package.IsValid())
				{
					Action.Package->SetDirtyFlag(
						Action.bPackageWasDirty);
				}
				return bDestroyed;
			}
			if (Action.Carrier == ECarrierKind::DynamicMeshActor)
			{
				ADynamicMeshActor* Actor =
					Action.DynamicActor.Get();
				UDynamicMeshComponent* Component =
					Actor
						? Actor->GetDynamicMeshComponent()
						: nullptr;
				if (!Actor
					|| Actor->GetWorld() != &World
					|| !Component
					|| !Component->GetDynamicMesh())
				{
					return false;
				}
				Component->GetDynamicMesh()->EditMesh(
					[&Action](FDynamicMesh3& Mesh)
					{
						Mesh = Action.OriginalMesh;
					});
				TArray<UMaterialInterface*> Materials;
				for (UMaterialInterface* Material
					: Action.OriginalDynamicMaterials)
				{
					Materials.Add(Material);
				}
				Component->ConfigureMaterialSet(Materials, true);
				Component->SetSimpleCollisionShapes(
					Action.OriginalCollision,
					false);
				Component->GetBodySetup()->AggGeom =
					Action.OriginalCollision;
				Component->NotifyMeshUpdated();
			}
			else
			{
				UStaticMesh* StaticMesh = Action.StaticMesh.Get();
				if (!StaticMesh
					|| !CommitStaticMesh(
						*StaticMesh,
						Action.OriginalMesh))
				{
					return false;
				}
				StaticMesh->GetStaticMaterials() =
					Action.OriginalStaticMaterials;
				if (Action.bHadCollision)
				{
					if (!StaticMesh->GetBodySetup())
					{
						StaticMesh->CreateBodySetup();
					}
					StaticMesh->GetBodySetup()->AggGeom =
						Action.OriginalCollision;
				}
				else if (StaticMesh->GetBodySetup())
				{
					StaticMesh->GetBodySetup()->AggGeom =
						FKAggregateGeom();
				}
			}
			if (Action.Package.IsValid())
			{
				Action.Package->SetDirtyFlag(
					Action.bPackageWasDirty);
			}
			return true;
		}

		bool RollbackAll(
			UWorld& World,
			TArray<FModelingGeometryAction>& Actions)
		{
			bool bConfirmed = true;
			for (int32 Index = Actions.Num() - 1;
				Index >= 0;
				--Index)
			{
				bConfirmed =
					RollbackAction(World, Actions[Index])
					&& bConfirmed;
			}
			return bConfirmed;
		}

		class FModelingGeometryEffectNativeHook final
			: public IUeAgentModelingGeometryEffectNativeHook
		{
		public:
			explicit FModelingGeometryEffectNativeHook(
				TSharedRef<
					IUeAgentModelingGeometryEffectReferenceResolver,
					ESPMode::ThreadSafe> InResolver)
				: Resolver(MoveTemp(InResolver))
			{
			}

			bool TryApply(
				UWorld& World,
				const FExecutionMutationRequest& Request,
				const FLockFenceLease& Lease,
				const TArray<FUeAgentGenericEffectBinding>& Bindings,
				FUeAgentResourceTracker& Resources,
				FUeAgentGenericEffectApplyResult& OutResult,
				FExecutionMutationError& OutError) override
			{
				OutResult = FUeAgentGenericEffectApplyResult();
				OutError = FExecutionMutationError();
				if (!IsInGameThread())
				{
					return Fail(
						OutError,
						TEXT(
							"modeling_geometry_game_thread_required"));
				}
				if (!Request.IsValid()
					|| !Lease.IsValid()
					|| Request.GetExecutionContext()
							.GetWorldIdentity()
						!= World.GetPathName())
				{
					return Fail(
						OutError,
						TEXT(
							"modeling_geometry_exact_world_context_mismatch"));
				}
				const TArray<FPlannedChange>& Changes =
					Request.GetPlan()
						->GetChangeSet()
						.GetChanges();
				if (Changes.IsEmpty()
					|| Bindings.Num() != Changes.Num())
				{
					return Fail(
						OutError,
						TEXT(
							"modeling_geometry_change_binding_mismatch"));
				}

				TArray<FModelingGeometryAction> Actions;
				Actions.Reserve(Changes.Num());
				for (const FPlannedChange& Change : Changes)
				{
					FModelingGeometryAction Action;
					const FString& TargetKind =
						Change.GetIdentity().GetTargetKind();
					const bool bPrepared =
						TargetKind
							== TEXT("geometry.primitive.create")
						? PreparePrimitive(
							Change,
							Action,
							OutError)
						: (TargetKind
								== TEXT("geometry.topology.edit")
							|| TargetKind
								== TEXT("geometry.attribute.edit")
							|| TargetKind
								== TEXT("geometry.collision.edit"))
							? CheckoutExistingCarrier(
								World,
								*Resolver,
								Change,
								Action,
								OutError)
							: Fail(
								OutError,
								TEXT(
									"modeling_geometry_target_kind_unsupported"),
								TargetKind);
					if (!bPrepared)
					{
						return false;
					}
					Actions.Add(MoveTemp(Action));
				}

				TArray<FModelingGeometryAction> AppliedActions;
				AppliedActions.Reserve(Actions.Num());
				TArray<FString> MutatedKeys;
				for (FModelingGeometryAction& Action : Actions)
				{
					if (!CommitAction(
							Action,
							*Resolver,
							World,
							OutError))
					{
						RollbackAll(World, AppliedActions);
						return false;
					}
					MutatedKeys.Add(Action.CanonicalKey);
					AppliedActions.Add(MoveTemp(Action));
				}

				const FString Token = FString::Printf(
					TEXT("modeling-geometry:%s:%lld"),
					*Request.GetOperationId(),
					Lease.GetFencingToken());
				{
					FScopeLock Lock(&StateMutex);
					RollbackByToken.Add(Token, AppliedActions);
					TokenByOperation.Add(
						Request.GetOperationId(),
						Token);
				}
				OutResult = FUeAgentGenericEffectApplyResult(
					Lease.GetFencingToken(),
					MoveTemp(MutatedKeys),
					{TEXT("mesh.authoring.memory")},
					true,
					Token,
					true);
				return OutResult.IsValid()
					|| Fail(
						OutError,
						TEXT(
							"modeling_geometry_apply_result_invalid"));
			}

			bool TryRollback(
				UWorld& World,
				const FExecutionMutationRequest& Request,
				const FUeAgentGenericEffectApplyResult& Applied,
				FUeAgentResourceTracker& Resources,
				bool& bOutConfirmed,
				FExecutionMutationError& OutError) override
			{
				bOutConfirmed = false;
				OutError = FExecutionMutationError();
				if (!IsInGameThread()
					|| !Applied.IsValid()
					|| Request.GetExecutionContext()
							.GetWorldIdentity()
						!= World.GetPathName())
				{
					return Fail(
						OutError,
						TEXT(
							"modeling_geometry_rollback_request_invalid"));
				}
				TArray<FModelingGeometryAction> Actions;
				{
					FScopeLock Lock(&StateMutex);
					const TArray<FModelingGeometryAction>* Found =
						RollbackByToken.Find(
							Applied.GetRollbackToken());
					if (!Found)
					{
						return Fail(
							OutError,
							TEXT(
								"modeling_geometry_rollback_token_unknown"));
					}
					Actions = *Found;
				}
				bOutConfirmed = RollbackAll(World, Actions);
				if (!bOutConfirmed)
				{
					return Fail(
						OutError,
						TEXT(
							"modeling_geometry_rollback_unconfirmed"));
				}
				{
					FScopeLock Lock(&StateMutex);
					RollbackByToken.Remove(
						Applied.GetRollbackToken());
					TokenByOperation.Remove(
						Request.GetOperationId());
				}
				return true;
			}

			void ReleaseVerifiedOperation(
				const FString& OperationId) override
			{
				FScopeLock Lock(&StateMutex);
				if (const FString* Token =
					TokenByOperation.Find(OperationId))
				{
					RollbackByToken.Remove(*Token);
				}
				TokenByOperation.Remove(OperationId);
			}

			bool TryWaitForSettle(
				const FBarrierRequest& Request,
				FUeAgentResourceTracker& Resources,
				FBarrierResult& OutResult,
				FExecutionMutationError& OutError) override
			{
				OutError = FExecutionMutationError();
				if (!Request.IsValid())
				{
					return Fail(
						OutError,
						TEXT(
							"modeling_geometry_barrier_request_invalid"));
				}
				OutResult = FBarrierResult(
					Request.GetSubjectGeneration(),
					Request.GetRequiredSignals(),
					FString::Printf(
						TEXT("modeling-geometry:%lld"),
						Request.GetSubjectGeneration()),
					true);
				return OutResult.IsValid()
					|| Fail(
						OutError,
						TEXT(
							"modeling_geometry_barrier_result_invalid"));
			}

		private:
			TSharedRef<
				IUeAgentModelingGeometryEffectReferenceResolver,
				ESPMode::ThreadSafe> Resolver;
			FCriticalSection StateMutex;
			TMap<
				FString,
				TArray<FModelingGeometryAction>> RollbackByToken;
			TMap<FString, FString> TokenByOperation;
		};
	}

	bool FUeAgentModelingGeometryEffectNativeHookFactory::TryCreate(
		TSharedRef<
			IUeAgentModelingGeometryEffectReferenceResolver,
			ESPMode::ThreadSafe> Resolver,
		TSharedPtr<IUeAgentModelingGeometryEffectNativeHook>& OutHook,
		FExecutionMutationError& OutError)
	{
		OutHook = MakeShared<FModelingGeometryEffectNativeHook>(
			MoveTemp(Resolver));
		OutError = FExecutionMutationError();
		return OutHook.IsValid();
	}
}

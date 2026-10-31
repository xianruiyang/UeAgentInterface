#include "DomainAdapters/ModelingGeometryEffect/UeAgentModelingGeometryEffectCapabilities.h"
#include "DomainAdapters/ModelingGeometryEffect/UeAgentModelingGeometryEffectBoundarySemantics.h"
#include "DomainAdapters/ModelingGeometryEffect/UeAgentModelingGeometryEffectBoolean.h"
#include "DomainAdapters/ModelingGeometry/UeAgentModelingMetricMaterial.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Containers/BitArray.h"

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
			bool bMutationStarted = false;
			FTransform Placement = FTransform::Identity;
			TStrongObjectPtr<UMaterialInterface> PrimitiveMaterial;
			TOptional<FLinearColor> PrimitiveColor;
		};

		TSharedPtr<FJsonValue> NativeNumberFact(const double Value)
		{
			return FMath::IsFinite(Value) ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(Value))
				: TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(FString::Printf(TEXT("%.17g"), Value)));
		}

		TSharedPtr<FJsonValue> NativeNumberArrayFact(const TArray<double>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> Items;
			for (const double Value : Values) Items.Add(NativeNumberFact(Value));
			return MakeShared<FJsonValueArray>(MoveTemp(Items));
		}

		TSharedPtr<FJsonValue> NativeValueFact(const FUeAgentCanonicalValuePtr& Value, const bool bIncludeScalar = true)
		{
			if (!Value.IsValid()) return MakeShared<FJsonValueNull>();
			const TSharedRef<FJsonObject> Shape = MakeShared<FJsonObject>();
			const TCHAR* Kind = TEXT("unknown");
			switch (Value->GetKind())
			{
			case EUeAgentValueKind::Null: Kind = TEXT("null"); break;
			case EUeAgentValueKind::Boolean:
				if (bIncludeScalar) return MakeShared<FJsonValueBoolean>(Value->GetBoolean());
				Kind = TEXT("boolean"); break;
			case EUeAgentValueKind::String:
				if (bIncludeScalar) return MakeShared<FJsonValueString>(Value->GetText());
				Kind = TEXT("string"); break;
			case EUeAgentValueKind::Integer: Kind = TEXT("integer"); if (bIncludeScalar) Shape->SetStringField(TEXT("value"), Value->GetLexeme()); break;
			case EUeAgentValueKind::Decimal: Kind = TEXT("decimal"); if (bIncludeScalar) Shape->SetStringField(TEXT("value"), Value->GetLexeme()); break;
			case EUeAgentValueKind::Array: Kind = TEXT("array"); Shape->SetNumberField(TEXT("count"), Value->GetArrayItems().Num()); break;
			case EUeAgentValueKind::Object: Kind = TEXT("object"); Shape->SetNumberField(TEXT("count"), Value->GetObjectItems().Num()); break;
			case EUeAgentValueKind::Temporal: Kind = TEXT("temporal"); break;
			case EUeAgentValueKind::Spatial: Kind = TEXT("spatial"); break;
			}
			Shape->SetStringField(TEXT("kind"), Kind);
			return MakeShared<FJsonValueObject>(Shape);
		}

		FExecutionMutationError NativeFact(const FString& Code, const FString& Reason, const FString& Path,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected, const FString& Target = FString())
		{
			FExecutionMutationError Error = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Path,
				MoveTemp(Expected), MoveTemp(Actual), TEXT("execution"), Reason);
			Error.SourceKind = TEXT("native_result"); Error.Source.JsonPath.Reset(); Error.SemanticPointer.Reset(); Error.TargetPath = Target;
			return Error;
		}

		bool ReadFailure(FExecutionMutationError& OutError, const FString& Code, const FString& Reason,
			const FString& Path, const FUeAgentCanonicalValuePtr& Value, const FString& Expected, const bool bPrivateValue = false)
		{
			OutError = NativeFact(Code, Reason, Path, NativeValueFact(Value, !bPrivateValue), MakeShared<FJsonValueString>(Expected));
			if (bPrivateValue) OutError.ActualEvidence = NativeValueFact(Value);
			return false;
		}

		FString NativeFieldPath(const FString& Parent, const FString& Field)
		{
			return Parent + TEXT("/") + Field.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
		}

		void SupplementNativeTarget(FExecutionMutationError& Error, const FString& Target)
		{
			if (Error.TargetPath.IsEmpty()) Error.TargetPath = Target;
			for (FExecutionMutationError& Child : Error.Diagnostics) SupplementNativeTarget(Child, Target);
			if (Error.PublicCause.IsValid())
			{
				FExecutionMutationError Cause = *Error.PublicCause;
				SupplementNativeTarget(Cause, Target);
				Error.PublicCause = MakeShared<FExecutionMutationError>(MoveTemp(Cause));
			}
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

		bool TryReadField(const FUeAgentCanonicalValuePtr& Object, const FString& Field, const FString& ParentPath,
			FExecutionMutationError* OutError, FUeAgentCanonicalValuePtr& OutValue)
		{
			if (!Object.IsValid() || Object->GetKind() != EUeAgentValueKind::Object)
			{
				if (OutError)
				{
					*OutError = NativeFact(TEXT("modeling_geometry_parameter_container_invalid"),
						TEXT("The canonical modeling parameter container must be an object."), ParentPath,
						NativeValueFact(Object, false), MakeShared<FJsonValueString>(TEXT("object")));
					OutError->ActualEvidence = NativeValueFact(Object);
				}
				return false;
			}
			OutValue = FindField(Object, Field); return true;
		}

		bool ReadString(
			const FUeAgentCanonicalValuePtr& Object, const FString& Field, FString& OutValue,
			FExecutionMutationError& OutError, const FString& ParentPath = TEXT("/after/typed_parameters"), const bool bPrivateValue = false)
		{
			OutValue.Reset();
			FUeAgentCanonicalValuePtr Value;
			if (!TryReadField(Object, Field, ParentPath, &OutError, Value)) return false;
			if (!Value.IsValid() || Value->GetKind() != EUeAgentValueKind::String)
				return ReadFailure(OutError, TEXT("modeling_geometry_internal_field_type"),
					FString::Printf(TEXT("The canonical modeling field '%s' must be a string."), *Field),
					NativeFieldPath(ParentPath, Field), Value, TEXT("non-empty string"), bPrivateValue);
			OutValue = Value->GetText();
			return !OutValue.IsEmpty() || ReadFailure(OutError, TEXT("modeling_geometry_internal_field_empty"),
				FString::Printf(TEXT("The canonical modeling field '%s' must not be empty."), *Field),
				NativeFieldPath(ParentPath, Field), Value, TEXT("non-empty string"), bPrivateValue);
		}

		bool ReadNumberValue(const FUeAgentCanonicalValuePtr& Object, const FString& Field, double& OutValue,
			FExecutionMutationError* OutError, const FString& ParentPath)
		{
			OutValue = 0.0;
			FUeAgentCanonicalValuePtr Value;
			if (!TryReadField(Object, Field, ParentPath, OutError, Value)) return false;
			if (!Value.IsValid() || (Value->GetKind() != EUeAgentValueKind::Decimal && Value->GetKind() != EUeAgentValueKind::Integer))
				return OutError && ReadFailure(*OutError, TEXT("modeling_geometry_internal_field_type"),
					FString::Printf(TEXT("The canonical modeling field '%s' must be numeric."), *Field),
					NativeFieldPath(ParentPath, Field), Value, TEXT("finite integer or decimal"));
			OutValue = FCString::Atod(*Value->GetLexeme());
			if (!FMath::IsFinite(OutValue))
				return OutError && ReadFailure(*OutError, TEXT("modeling_geometry_number_non_finite"),
					TEXT("The modeling parameter is not a finite number."), NativeFieldPath(ParentPath, Field), Value, TEXT("finite number"));
			return true;
		}

		bool ReadNumber(const FUeAgentCanonicalValuePtr& Object, const FString& Field, double& OutValue,
			FExecutionMutationError& OutError, const FString& ParentPath = TEXT("/after/typed_parameters"))
		{
			return ReadNumberValue(Object, Field, OutValue, &OutError, ParentPath);
		}

		bool ReadNumberProbe(const FUeAgentCanonicalValuePtr& Object, const FString& Field, double& OutValue)
		{
			return ReadNumberValue(Object, Field, OutValue, nullptr, FString());
		}

		bool ReadInteger(const FUeAgentCanonicalValuePtr& Object, const FString& Field, int32& OutValue,
			FExecutionMutationError& OutError, const FString& ParentPath = TEXT("/after/typed_parameters"))
		{
			double Number = 0.0;
			if (!ReadNumber(Object, Field, Number, OutError, ParentPath)) return false;
			if (Number < static_cast<double>(MIN_int32) || Number > static_cast<double>(MAX_int32))
				return ReadFailure(OutError, TEXT("modeling_geometry_integer_out_of_range"), TEXT("The modeling integer parameter is outside the int32 range."),
					NativeFieldPath(ParentPath, Field), FindField(Object, Field), TEXT("integer between -2147483648 and 2147483647"));
			OutValue = FMath::RoundToInt(Number);
			return FMath::IsNearlyEqual(Number, static_cast<double>(OutValue), UE_DOUBLE_SMALL_NUMBER)
				|| ReadFailure(OutError, TEXT("modeling_geometry_integer_fractional"), TEXT("The modeling parameter must be an integer within the numeric tolerance."),
					NativeFieldPath(ParentPath, Field), FindField(Object, Field),
					FString::Printf(TEXT("integer within %.17g"), static_cast<double>(UE_DOUBLE_SMALL_NUMBER)));
		}

		bool ReadElementId(const FUeAgentCanonicalValueRef& Value, int32& OutValue,
			FExecutionMutationError& OutError, const FString& Path)
		{
			if (Value->GetKind() != EUeAgentValueKind::Integer && Value->GetKind() != EUeAgentValueKind::Decimal)
				return ReadFailure(OutError, TEXT("modeling_geometry_element_id_invalid"), TEXT("The element identifier must be numeric."),
					Path, FUeAgentCanonicalValuePtr(Value), TEXT("integer between 0 and 2147483647"));
			const double Number = FCString::Atod(*Value->GetLexeme());
			if (!FMath::IsFinite(Number) || Number < 0.0 || Number > static_cast<double>(MAX_int32))
				return ReadFailure(OutError, TEXT("modeling_geometry_element_id_invalid"), TEXT("The element identifier is not a finite non-negative int32 value."),
					Path, FUeAgentCanonicalValuePtr(Value), TEXT("integer between 0 and 2147483647"));
			OutValue = FMath::RoundToInt(Number);
			return FMath::IsNearlyEqual(Number, static_cast<double>(OutValue), UE_DOUBLE_SMALL_NUMBER)
				|| ReadFailure(OutError, TEXT("modeling_geometry_element_id_invalid"), TEXT("The element identifier must be integral within the numeric tolerance."),
					Path, FUeAgentCanonicalValuePtr(Value), FString::Printf(TEXT("integer within %.17g"), static_cast<double>(UE_DOUBLE_SMALL_NUMBER)));
		}

		bool ReadBoolean(const FUeAgentCanonicalValuePtr& Object, const FString& Field, bool& OutValue,
			FExecutionMutationError& OutError, const FString& ParentPath = TEXT("/after/typed_parameters"))
		{
			FUeAgentCanonicalValuePtr Value;
			if (!TryReadField(Object, Field, ParentPath, &OutError, Value)) return false;
			if (!Value.IsValid() || Value->GetKind() != EUeAgentValueKind::Boolean)
				return ReadFailure(OutError, TEXT("modeling_geometry_internal_field_type"),
					FString::Printf(TEXT("The modeling field '%s' must be boolean."), *Field),
					NativeFieldPath(ParentPath, Field), Value, TEXT("boolean"));
			OutValue = Value->GetBoolean(); return true;
		}

		bool ReadNumberArray(const FUeAgentCanonicalValuePtr& Object, const FString& Field, const int32 Count,
			TArray<double>& OutValues, FExecutionMutationError& OutError, const FString& ParentPath = TEXT("/after/typed_parameters"))
		{
			OutValues.Reset();
			FUeAgentCanonicalValuePtr Value;
			if (!TryReadField(Object, Field, ParentPath, &OutError, Value)) return false;
			const FString Path = NativeFieldPath(ParentPath, Field);
			if (!Value.IsValid() || Value->GetKind() != EUeAgentValueKind::Array || Value->GetArrayItems().Num() != Count)
				return ReadFailure(OutError, TEXT("modeling_geometry_number_array_invalid"),
					FString::Printf(TEXT("The modeling field '%s' must contain exactly %d finite numbers."), *Field, Count),
					Path, Value, FString::Printf(TEXT("array of %d finite numbers"), Count));
			const TArray<FUeAgentCanonicalValueRef>& Items = Value->GetArrayItems();
			for (int32 Index = 0; Index < Items.Num(); ++Index)
			{
				const FUeAgentCanonicalValueRef& Item = Items[Index];
				const FString ItemPath = Path + FString::Printf(TEXT("/%d"), Index);
				if (Item->GetKind() != EUeAgentValueKind::Integer && Item->GetKind() != EUeAgentValueKind::Decimal)
					return ReadFailure(OutError, TEXT("modeling_geometry_number_array_invalid"), TEXT("This modeling array component must be numeric."),
						ItemPath, FUeAgentCanonicalValuePtr(Item), TEXT("finite integer or decimal"));
				const double Number = FCString::Atod(*Item->GetLexeme());
				if (!FMath::IsFinite(Number))
					return ReadFailure(OutError, TEXT("modeling_geometry_number_array_invalid"), TEXT("This modeling array component must be finite."),
						ItemPath, FUeAgentCanonicalValuePtr(Item), TEXT("finite number"));
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

		bool ReadIntegerArray(const FUeAgentCanonicalValuePtr& Object, const FString& Field, const int32 Count,
			TArray<int32>& OutValues, FExecutionMutationError& OutError, const FString& ParentPath = TEXT("/after/typed_parameters"))
		{
			TArray<double> Numbers;
			if (!ReadNumberArray(Object, Field, Count, Numbers, OutError, ParentPath)) return false;
			OutValues.Reset(Count);
			const FUeAgentCanonicalValuePtr Array = FindField(Object, Field);
			for (int32 Index = 0; Index < Numbers.Num(); ++Index)
			{
				const double Number = Numbers[Index];
				const FString Path = NativeFieldPath(ParentPath, Field) + FString::Printf(TEXT("/%d"), Index);
				if (Number < static_cast<double>(MIN_int32) || Number > static_cast<double>(MAX_int32))
					return ReadFailure(OutError, TEXT("modeling_geometry_integer_array_invalid"), TEXT("This modeling array integer is outside the int32 range."),
						Path, FUeAgentCanonicalValuePtr(Array->GetArrayItems()[Index]), TEXT("integer between -2147483648 and 2147483647"));
				const int32 Value = FMath::RoundToInt(Number);
				if (!FMath::IsNearlyEqual(Number, static_cast<double>(Value), UE_DOUBLE_SMALL_NUMBER))
					return ReadFailure(OutError, TEXT("modeling_geometry_integer_array_invalid"), TEXT("This modeling array component must be integral within the numeric tolerance."),
						Path, FUeAgentCanonicalValuePtr(Array->GetArrayItems()[Index]),
						FString::Printf(TEXT("integer within %.17g"), static_cast<double>(UE_DOUBLE_SMALL_NUMBER)));
				OutValues.Add(Value);
			}
			return true;
		}

		bool ReadElementIds(const FUeAgentCanonicalValuePtr& Object, const FString& Field, TArray<int32>& OutValues,
			FExecutionMutationError& OutError, const FString& ParentPath = TEXT("/after/typed_parameters"))
		{
			OutValues.Reset();
			FUeAgentCanonicalValuePtr Value;
			if (!TryReadField(Object, Field, ParentPath, &OutError, Value)) return false;
			const FString Path = NativeFieldPath(ParentPath, Field);
			if (!Value.IsValid() || Value->GetKind() != EUeAgentValueKind::Array || Value->GetArrayItems().IsEmpty())
				return ReadFailure(OutError, TEXT("modeling_geometry_element_ids_invalid"),
					TEXT("The modeling element identifiers must be a non-empty array."), Path, Value, TEXT("non-empty array of non-negative integer identifiers"));
			const TArray<FUeAgentCanonicalValueRef>& Items = Value->GetArrayItems();
			for (int32 Index = 0; Index < Items.Num(); ++Index)
			{
				int32 Parsed = INDEX_NONE;
				if (!ReadElementId(Items[Index], Parsed, OutError, Path + FString::Printf(TEXT("/%d"), Index))) return false;
				OutValues.Add(Parsed);
			}
			return true;
		}

		TStrongObjectPtr<UDynamicMesh> MakeTransientMesh(
			const FDynamicMesh3& Mesh,
			const FString& OperationKey,
			FExecutionMutationError& OutError)
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
			else
			{
				OutError = NativeFact(TEXT("modeling_geometry_transient_mesh_failed"),
					FString::Printf(TEXT("Native dynamic mesh allocation failed for modeling operation '%s'."), *OperationKey),
					TEXT("/transient_mesh/available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
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

		TSharedPtr<FJsonValue> NativeMeshCountsFact(const FDynamicMesh3& Mesh)
		{
			const TSharedRef<FJsonObject> Counts = MakeShared<FJsonObject>();
			Counts->SetNumberField(TEXT("vertices"), Mesh.VertexCount());
			Counts->SetNumberField(TEXT("triangles"), Mesh.TriangleCount());
			return MakeShared<FJsonValueObject>(Counts);
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
			if (!bCopied)
			{
				OutError = NativeFact(TEXT("modeling_geometry_kernel_result_invalid"),
					TEXT("The native mesh result could not be read because its mesh callback was not invoked."),
					TEXT("/mesh_result/read_callback_invoked"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				return false;
			}
			if (OutMesh.TriangleCount() <= 0 || OutMesh.VertexCount() <= 0)
			{
				OutError = NativeFact(TEXT("modeling_geometry_kernel_result_invalid"),
					TEXT("The native mesh result is empty."), TEXT("/mesh_result/counts"),
					NativeMeshCountsFact(OutMesh), MakeShared<FJsonValueString>(TEXT("positive vertex and triangle counts")));
				return false;
			}
			return true;
		}

		bool CutMeshHalf(
			FDynamicMesh3& Mesh,
			const FVector3d& Origin,
			const FVector3d& Normal,
			const bool bKeepPositiveSide,
			const bool bFill,
			FExecutionMutationError& OutError)
		{
			const auto SetCutFailure = [&](const bool bHoleFill)
			{
				const TSharedRef<FJsonObject> Inputs = MakeShared<FJsonObject>();
				Inputs->SetStringField(TEXT("keep_side"), bKeepPositiveSide ? TEXT("positive") : TEXT("negative"));
				Inputs->SetBoolField(TEXT("fill_hole"), bFill);
				Inputs->SetField(TEXT("origin_cm"), NativeNumberArrayFact({Origin.X, Origin.Y, Origin.Z}));
				Inputs->SetField(TEXT("normal"), NativeNumberArrayFact({Normal.X, Normal.Y, Normal.Z}));
				OutError = NativeFact(TEXT("modeling_geometry_plane_cut_failed"),
					bHoleFill ? TEXT("The native plane cut succeeded, but filling its cut boundary failed.")
						: TEXT("The native plane cut failed on the requested side."),
					bHoleFill ? TEXT("/plane_cut/hole_fill") : TEXT("/plane_cut/cut"),
					MakeShared<FJsonValueObject>(Inputs), MakeShared<FJsonValueString>(TEXT("operation succeeds on the requested side")));
			};
			UE::Geometry::FMeshPlaneCut Cut(
				&Mesh,
				Origin,
				bKeepPositiveSide ? -Normal : Normal);
			if (!Cut.Cut())
			{
				SetCutFailure(false);
				return false;
			}
			if (bFill && !Cut.MinimalHoleFill())
			{
				SetCutFailure(true);
				return false;
			}
			return true;
		}

		bool ReadTransform(
			const FUeAgentCanonicalValuePtr& Object, const FString& TranslationField, const FString& RotationField,
			const FString& ScaleField, FTransform& OutTransform, FExecutionMutationError& OutError,
			const FString& ParentPath = TEXT("/after/typed_parameters"))
		{
			TArray<double> Translation;
			TArray<double> Rotation;
			TArray<double> Scale;
			if (!ReadNumberArray(Object, TranslationField, 3, Translation, OutError, ParentPath)
				|| !ReadNumberArray(Object, RotationField, 4, Rotation, OutError, ParentPath)
				|| !ReadNumberArray(Object, ScaleField, 3, Scale, OutError, ParentPath)) return false;
			double UnitScale = 1.0;
			const FUeAgentCanonicalValuePtr Unit = FindField(Object, TEXT("unit"));
			if (Unit.IsValid() && Unit->GetKind() == EUeAgentValueKind::String)
			{
				if (Unit->GetText() == TEXT("meter")) UnitScale = 100.0;
				else if (Unit->GetText() != TEXT("centimeter"))
					return ReadFailure(OutError, TEXT("modeling_geometry_transform_unit_invalid"),
						TEXT("The modeling transform unit is not supported."), NativeFieldPath(ParentPath, TEXT("unit")),
						Unit, TEXT("meter or centimeter"));
			}
			FQuat Quaternion(Rotation[0], Rotation[1], Rotation[2], Rotation[3]);
			if (Quaternion.SizeSquared() <= UE_DOUBLE_SMALL_NUMBER)
			{
				OutError = NativeFact(TEXT("modeling_geometry_transform_rotation_invalid"),
					TEXT("The modeling transform quaternion has a near-zero norm."), NativeFieldPath(ParentPath, RotationField),
					NativeNumberArrayFact(Rotation), MakeShared<FJsonValueString>(FString::Printf(TEXT("quaternion norm squared greater than %.17g"), static_cast<double>(UE_DOUBLE_SMALL_NUMBER))));
				return false;
			}
			Quaternion.Normalize();
			OutTransform = FTransform(Quaternion, FVector(Translation[0] * UnitScale, Translation[1] * UnitScale, Translation[2] * UnitScale),
				FVector(Scale[0], Scale[1], Scale[2]));
			return true;
		}

		bool ReadPlane(const FUeAgentCanonicalValuePtr& Parameters, FVector3d& OutOrigin, FVector3d& OutNormal,
			FExecutionMutationError& OutError, const FString& ParentPath = TEXT("/after/typed_parameters"))
		{
			const FString Path = NativeFieldPath(ParentPath, TEXT("plane"));
			const FUeAgentCanonicalValuePtr Plane = FindField(Parameters, TEXT("plane"));
			TArray<double> Origin;
			TArray<double> Normal;
			if (!ReadNumberArray(Plane, TEXT("origin"), 3, Origin, OutError, Path)
				|| !ReadNumberArray(Plane, TEXT("normal"), 3, Normal, OutError, Path)) return false;
			double UnitScale = 1.0;
			const FUeAgentCanonicalValuePtr Unit = FindField(Plane, TEXT("unit"));
			if (Unit.IsValid() && Unit->GetKind() == EUeAgentValueKind::String && Unit->GetText() == TEXT("meter")) UnitScale = 100.0;
			OutOrigin = FVector3d(Origin[0] * UnitScale, Origin[1] * UnitScale, Origin[2] * UnitScale);
			OutNormal = FVector3d(Normal[0], Normal[1], Normal[2]);
			if (!OutNormal.Normalize())
			{
				OutError = NativeFact(TEXT("modeling_geometry_plane_normal_invalid"), TEXT("The modeling plane normal cannot be normalized."),
					NativeFieldPath(Path, TEXT("normal")), NativeNumberArrayFact(Normal), MakeShared<FJsonValueString>(TEXT("non-zero normal under the geometry normalization tolerance")));
				return false;
			}
			return true;
		}

		bool ReadDirection(const FUeAgentCanonicalValuePtr& Parameters, FVector3d& OutDirection,
			FExecutionMutationError& OutError, const FString& ParentPath = TEXT("/after/typed_parameters"))
		{
			const FString Path = NativeFieldPath(ParentPath, TEXT("direction"));
			const FUeAgentCanonicalValuePtr Direction = FindField(Parameters, TEXT("direction"));
			TArray<double> Vector;
			if (!ReadNumberArray(Direction, TEXT("vector"), 3, Vector, OutError, Path)) return false;
			OutDirection = FVector3d(Vector[0], Vector[1], Vector[2]);
			if (!OutDirection.Normalize())
			{
				OutError = NativeFact(TEXT("modeling_geometry_direction_invalid"), TEXT("The modeling direction vector cannot be normalized."),
					NativeFieldPath(Path, TEXT("vector")), NativeNumberArrayFact(Vector), MakeShared<FJsonValueString>(TEXT("non-zero vector under the geometry normalization tolerance")));
				return false;
			}
			return true;
		}

		bool BuildSelection(UDynamicMesh& DynamicMesh, const FUeAgentCanonicalValuePtr& Parameters,
			FGeometryScriptMeshSelection& OutSelection, FExecutionMutationError& OutError)
		{
			const FString Path = TEXT("/after/typed_parameters/selection");
			const FUeAgentCanonicalValuePtr Selection = FindField(Parameters, TEXT("selection"));
			FString Kind;
			TArray<int32> ElementIds;
			if (!ReadString(Selection, TEXT("element_kind"), Kind, OutError, Path)
				|| !ReadElementIds(Selection, TEXT("element_ids"), ElementIds, OutError, Path)) return false;
			const EGeometryScriptMeshSelectionType SelectionType = Kind == TEXT("triangle") ? EGeometryScriptMeshSelectionType::Triangles
				: Kind == TEXT("vertex") ? EGeometryScriptMeshSelectionType::Vertices : EGeometryScriptMeshSelectionType::Edges;
			bool bValid = true;
			int32 InvalidIndex = INDEX_NONE;
			DynamicMesh.ProcessMesh([&](const FDynamicMesh3& Mesh)
			{
				for (int32 Index = 0; Index < ElementIds.Num(); ++Index)
				{
					const int32 ElementId = ElementIds[Index];
					const bool bWasValid = bValid;
					bValid = bValid && (SelectionType == EGeometryScriptMeshSelectionType::Triangles ? Mesh.IsTriangle(ElementId)
						: SelectionType == EGeometryScriptMeshSelectionType::Vertices ? Mesh.IsVertex(ElementId) : Mesh.IsEdge(ElementId));
					if (Kind == TEXT("boundary_edge")) bValid = bValid && Mesh.IsBoundaryEdge(ElementId);
					if (bWasValid && !bValid) InvalidIndex = Index;
				}
			});
			if (!bValid)
			{
				OutError = NativeFact(TEXT("modeling_geometry_selection_stale"),
					FString::Printf(TEXT("The selected %s identifier is not valid in the current mesh."), *Kind),
					Path + FString::Printf(TEXT("/element_ids/%d"), InvalidIndex), MakeShared<FJsonValueNumber>(ElementIds[InvalidIndex]),
					MakeShared<FJsonValueString>(Kind == TEXT("boundary_edge") ? TEXT("existing boundary edge") : TEXT("existing element of the selected kind")));
				return false;
			}
			UGeometryScriptLibrary_MeshSelectionFunctions::ConvertIndexArrayToMeshSelection(&DynamicMesh, ElementIds, SelectionType, OutSelection);
			if (OutSelection.IsEmpty())
			{
				const TSharedRef<FJsonObject> Actual = MakeShared<FJsonObject>();
				Actual->SetNumberField(TEXT("input_identifier_count"), ElementIds.Num()); Actual->SetBoolField(TEXT("selection_empty"), true);
				OutError = NativeFact(TEXT("modeling_geometry_selection_empty"), TEXT("Unreal produced an empty geometry selection from these identifiers."),
					Path, MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(TEXT("non-empty geometry selection")));
				return false;
			}
			return true;
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
						SizeValues,
						OutError))
				{
					return false;
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
						StepCount,
						OutError)
					|| !ReadNumber(
						Parameters,
						TEXT("step_width_cm"),
						Width,
						OutError)
					|| !ReadNumber(
						Parameters,
						TEXT("step_depth_cm"),
						Depth,
						OutError)
					|| !ReadNumber(
						Parameters,
						TEXT("step_height_cm"),
						Height,
						OutError)
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
					return false;
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
				MakeTransientMesh(Mesh, OperationKey, OutError);
			if (!DynamicMesh.IsValid()) return false;
			if (OperationKey == TEXT("box"))
			{
				TArray<double> Size;
				TArray<int32> Subdivisions;
				FString Anchor;
				if (!ReadNumberArray(
						Parameters,
						TEXT("size_cm"),
						3,
						Size,
						OutError)
					|| !ReadIntegerArray(
						Parameters,
						TEXT("subdivisions"),
						3,
						Subdivisions,
						OutError)
					|| !ReadString(
						Parameters,
						TEXT("anchor"),
						Anchor,
						OutError))
				{
					return false;
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
						Radius,
						OutError)
					|| !ReadNumber(
						Parameters,
						TEXT("height_cm"),
						Height,
						OutError)
					|| !ReadInteger(
						Parameters,
						TEXT("radial_segments"),
						RadialSegments,
						OutError)
					|| !ReadInteger(
						Parameters,
						TEXT("height_segments"),
						HeightSegments,
						OutError)
					|| !ReadBoolean(
						Parameters,
						TEXT("cap_ends"),
						bCapped,
						OutError)
					|| !ReadString(
						Parameters,
						TEXT("anchor"),
						Anchor,
						OutError))
				{
					return false;
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
						Size,
						OutError)
					|| !ReadIntegerArray(
						Parameters,
						TEXT("subdivisions"),
						2,
						Subdivisions,
						OutError)
					|| !ReadBoolean(
						Parameters,
						TEXT("double_sided"),
						bDoubleSided,
						OutError))
				{
					return false;
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
						Radius,
						OutError)
					|| !ReadInteger(
						Parameters,
						TEXT("longitude_segments"),
						LongitudeSegments,
						OutError)
					|| !ReadInteger(
						Parameters,
						TEXT("latitude_segments"),
						LatitudeSegments,
						OutError))
				{
					return false;
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
				TArray<TSharedPtr<FJsonValue>> SupportedKeys;
				for (const TCHAR* Supported : {TEXT("ramp"), TEXT("ramp_corner"), TEXT("stairs"), TEXT("box"), TEXT("cylinder"), TEXT("plane"), TEXT("sphere")})
					SupportedKeys.Add(MakeShared<FJsonValueString>(Supported));
				OutError = NativeFact(TEXT("modeling_geometry_primitive_operation_unsupported"),
					TEXT("The modeling primitive operation is not supported."), TEXT("/operation_key"),
					MakeShared<FJsonValueString>(OperationKey), MakeShared<FJsonValueArray>(MoveTemp(SupportedKeys)));
				return false;
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
			const bool bPreserveSharpEdges,
			FExecutionMutationError& OutError)
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
				const TSharedRef<FJsonObject> Facts = MakeShared<FJsonObject>();
				Facts->SetField(TEXT("target_edge_length_cm"), NativeNumberFact(TargetEdgeLength));
				Facts->SetNumberField(TEXT("iterations"), Iterations);
				Facts->SetBoolField(TEXT("preserve_boundaries"), bPreserveBoundaries);
				Facts->SetBoolField(TEXT("preserve_sharp_edges"), bPreserveSharpEdges);
				Facts->SetBoolField(TEXT("result_available"), Result.IsValid());
				if (Result.IsValid()) Facts->SetField(TEXT("counts"), NativeMeshCountsFact(*Result));
				OutError = NativeFact(TEXT("modeling_geometry_remesh_failed"),
					Result.IsValid() ? TEXT("The native remesher returned a mesh with no triangles.")
						: TEXT("The native remesher did not return a mesh result."),
					TEXT("/remesh/result"), MakeShared<FJsonValueObject>(Facts),
					MakeShared<FJsonValueString>(TEXT("mesh result with at least one triangle")));
				Mesh = MoveTemp(*SourceMesh);
				return false;
			}
			Mesh = MoveTemp(*Result);
			Mesh.CompactInPlace();
			return true;
		}


		FExecutionMutationError BoundaryDifferenceFailure(
			const FBoundarySemanticObservation& Before,
			const FBoundarySemanticObservation& After,
			const FString& Target)
		{
			FString Reason = TEXT("Native simplification changed the boundary geometry.");
			FString Path = TEXT("/boundary/geometry_preserved");
			TSharedPtr<FJsonValue> Actual = MakeShared<FJsonValueBoolean>(false);
			TSharedPtr<FJsonValue> Expected = MakeShared<FJsonValueBoolean>(true);
			if (Before.EdgeCount != After.EdgeCount)
			{
				Reason = TEXT("Native simplification changed the boundary edge count.");
				Path = TEXT("/boundary/edge_count");
				Actual = MakeShared<FJsonValueNumber>(After.EdgeCount);
				Expected = MakeShared<FJsonValueNumber>(Before.EdgeCount);
			}
			else if (Before.VertexCount != After.VertexCount)
			{
				Reason = TEXT("Native simplification changed the boundary vertex count.");
				Path = TEXT("/boundary/vertex_count");
				Actual = MakeShared<FJsonValueNumber>(After.VertexCount);
				Expected = MakeShared<FJsonValueNumber>(Before.VertexCount);
			}
			else
			{
				const TCHAR* Axes[] = {TEXT("x"), TEXT("y"), TEXT("z")};
				bool bFound = false;
				for (int32 EndpointIndex = 0; EndpointIndex < FMath::Min(Before.OrderedEndpoints.Num(), After.OrderedEndpoints.Num()) && !bFound; ++EndpointIndex)
				{
					for (int32 AxisIndex = 0; AxisIndex < 3; ++AxisIndex)
					{
						const double BeforeValue = Before.OrderedEndpoints[EndpointIndex][AxisIndex];
						const double AfterValue = After.OrderedEndpoints[EndpointIndex][AxisIndex];
						if (BeforeValue == AfterValue) continue;
						Reason = TEXT("Native simplification moved a boundary segment endpoint (coordinates in centimeters).");
						Path = FString::Printf(TEXT("/boundary/segments/%d/%s/%s"), EndpointIndex / 2,
							EndpointIndex % 2 == 0 ? TEXT("a") : TEXT("b"), Axes[AxisIndex]);
						Actual = NativeNumberFact(AfterValue);
						Expected = NativeNumberFact(BeforeValue);
						bFound = true;
						break;
					}
				}
			}
			FExecutionMutationError Error = NativeFact(TEXT("modeling_geometry_simplify_boundary_not_preserved"),
				Reason, Path, MoveTemp(Actual), MoveTemp(Expected), Target);
			Error.ActualEvidence = MakeShared<FJsonValueString>(After.Fingerprint);
			Error.ExpectedEvidence = MakeShared<FJsonValueString>(Before.Fingerprint);
			return Error;
		}


		bool EditContainedComponents(FModelingGeometryAction& Action, FExecutionMutationError& OutError)
		{
			TArray<double> Minimum, Maximum, Translation;
			if (!ReadNumberArray(Action.Parameters, TEXT("bounds_min_cm"), 3, Minimum, OutError)
				|| !ReadNumberArray(Action.Parameters, TEXT("bounds_max_cm"), 3, Maximum, OutError)) return false;
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				if (Minimum[Axis] > Maximum[Axis])
				{
					OutError = NativeFact(TEXT("modeling_geometry_component_bounds_invalid"),
						TEXT("Component bounds must be ordered in target-local centimeters."),
						TEXT("/after/typed_parameters/bounds_min_cm"), NativeNumberArrayFact(Minimum),
						NativeNumberArrayFact(Maximum), Action.TargetRef);
					return false;
				}
			}
			const bool bTranslate = Action.OperationKey == TEXT("translate_components");
			if (bTranslate && !ReadNumberArray(Action.Parameters, TEXT("translation_cm"), 3, Translation, OutError)) return false;

			// A selection contains complete edge-connected triangle components.
			// Touching positions are not welded; a partly enclosed host wall is
			// excluded, rather than clipped into a new surface by a Boolean.
			FDynamicMesh3& Mesh = Action.WorkingMesh;
			TBitArray<> Visited(false, Mesh.MaxTriangleID());
			TArray<int32> SelectedTriangles;
			for (const int32 Seed : Mesh.TriangleIndicesItr())
			{
				if (Visited[Seed]) continue;
				TArray<int32> Component;
				Component.Add(Seed);
				Visited[Seed] = true;
				bool bContained = true;
				for (int32 Index = 0; Index < Component.Num(); ++Index)
				{
					const int32 TriangleId = Component[Index];
					const FIndex3i Vertices = Mesh.GetTriangle(TriangleId);
					for (int32 Corner = 0; Corner < 3; ++Corner)
					{
						const FVector3d Position = Mesh.GetVertex(Vertices[Corner]);
						for (int32 Axis = 0; Axis < 3; ++Axis)
							bContained &= Position[Axis] >= Minimum[Axis] && Position[Axis] <= Maximum[Axis];
					}
					const FIndex3i Neighbours = Mesh.GetTriNeighbourTris(TriangleId);
					for (int32 Edge = 0; Edge < 3; ++Edge)
					{
						const int32 Neighbour = Neighbours[Edge];
						if (Neighbour >= 0 && !Visited[Neighbour])
						{
							Visited[Neighbour] = true;
							Component.Add(Neighbour);
						}
					}
				}
				if (bContained) SelectedTriangles.Append(Component);
			}
			if (SelectedTriangles.IsEmpty())
			{
				OutError = NativeFact(TEXT("modeling_geometry_component_selection_empty"),
					TEXT("No complete edge-connected triangle component is contained in these target-local bounds."),
					TEXT("/after/typed_parameters/bounds_min_cm"), MakeShared<FJsonValueNumber>(0),
					MakeShared<FJsonValueString>(TEXT("at least one fully contained component")), Action.TargetRef);
				return false;
			}
			if (bTranslate)
			{
				TSet<int32> SelectedVertices;
				for (const int32 TriangleId : SelectedTriangles)
				{
					const FIndex3i Vertices = Mesh.GetTriangle(TriangleId);
					for (int32 Corner = 0; Corner < 3; ++Corner) SelectedVertices.Add(Vertices[Corner]);
				}
				const FVector3d Delta(Translation[0], Translation[1], Translation[2]);
				for (const int32 VertexId : SelectedVertices) Mesh.SetVertex(VertexId, Mesh.GetVertex(VertexId) + Delta);
			}
			else
			{
				for (const int32 TriangleId : SelectedTriangles)
				{
					if (Mesh.RemoveTriangle(TriangleId, true, false) != UE::Geometry::EMeshResult::Ok)
					{
						OutError = NativeFact(TEXT("modeling_geometry_component_remove_failed"),
							TEXT("Unreal could not remove a selected component triangle."), TEXT("/mesh/triangle_id"),
							MakeShared<FJsonValueNumber>(TriangleId), MakeShared<FJsonValueString>(TEXT("removable triangle")), Action.TargetRef);
						return false;
					}
				}
			}
			return true;
		}

		bool ApplyTopologyOperation(
			FModelingGeometryAction& Action,
			FExecutionMutationError& OutError)
		{
			const FString& Key = Action.OperationKey;
			if (Action.WorkingMesh.TriangleCount() == 0)
			{
				OutError = NativeFact(TEXT("modeling_geometry_mesh_empty"),
					TEXT("The topology operation requires a mesh with triangles."), TEXT("/mesh/triangle_count"),
					MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("greater than zero")), Action.TargetRef);
				return false;
			}

			if (Key == TEXT("remove_components") || Key == TEXT("translate_components"))
				return EditContainedComponents(Action, OutError);

			if (Key == TEXT("fill_holes"))
			{
				const FUeAgentCanonicalValuePtr Selection =
					FindField(Action.Parameters, TEXT("selection"));
				TArray<int32> SelectedEdges;
				FString FillMode;
				if (!ReadElementIds(
						Selection,
						TEXT("element_ids"),
						SelectedEdges,
						OutError,
						TEXT("/after/typed_parameters/selection"))
					|| !ReadString(
						Action.Parameters,
						TEXT("fill_mode"),
						FillMode,
						OutError))
				{
					return false;
				}
				TSharedPtr<FDynamicMesh3> Original =
					MakeShared<FDynamicMesh3>(Action.WorkingMesh);
				UE::Geometry::FMeshBoundaryLoops BoundaryLoops(
					Original.Get(),
					true);
				if (BoundaryLoops.bAborted)
				{
					OutError = NativeFact(TEXT("modeling_geometry_boundary_loops_invalid"),
						TEXT("Native boundary-loop detection aborted before hole filling."), TEXT("/boundary_loops/aborted"),
						MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), Action.TargetRef);
					return false;
				}
				TSet<int32> LoopIndices;
				for (int32 SelectionIndex = 0; SelectionIndex < SelectedEdges.Num(); ++SelectionIndex)
				{
					const int32 EdgeId = SelectedEdges[SelectionIndex];
					if (!Original->IsEdge(EdgeId)
						|| !Original->IsBoundaryEdge(EdgeId))
					{
						OutError = NativeFact(TEXT("modeling_geometry_boundary_selection_stale"),
							Original->IsEdge(EdgeId) ? TEXT("The selected edge is not a boundary edge.")
								: TEXT("The selected boundary edge does not exist in the current mesh."),
							FString::Printf(TEXT("/after/typed_parameters/selection/element_ids/%d"), SelectionIndex),
							MakeShared<FJsonValueNumber>(EdgeId), MakeShared<FJsonValueString>(TEXT("existing boundary edge ID")), Action.TargetRef);
						return false;
					}
					const int32 LoopIndex =
						BoundaryLoops.FindLoopContainingEdge(EdgeId);
					if (LoopIndex == INDEX_NONE)
					{
						OutError = NativeFact(TEXT("modeling_geometry_boundary_loop_missing"),
							TEXT("No detected boundary loop contains the selected edge."),
							FString::Printf(TEXT("/after/typed_parameters/selection/element_ids/%d"), SelectionIndex),
							MakeShared<FJsonValueNumber>(EdgeId), MakeShared<FJsonValueString>(TEXT("edge belonging to a detected boundary loop")), Action.TargetRef);
						return false;
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
					const TSharedRef<FJsonObject> Facts = MakeShared<FJsonObject>();
					Facts->SetBoolField(TEXT("result_available"), Result.IsValid());
					Facts->SetNumberField(TEXT("failed_loops"), HoleFiller.NumFailedLoops);
					Facts->SetNumberField(TEXT("selected_loops"), LoopIndices.Num());
					Facts->SetStringField(TEXT("fill_mode"), FillMode);
					OutError = NativeFact(TEXT("modeling_geometry_hole_fill_failed"),
						Result.IsValid() ? TEXT("The native hole filler could not fill all selected boundary loops.")
							: TEXT("The native hole filler did not return a mesh result."),
						TEXT("/hole_fill/result"), MakeShared<FJsonValueObject>(Facts),
						MakeShared<FJsonValueString>(TEXT("mesh result with zero failed loops")), Action.TargetRef);
					return false;
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
						Tolerance,
						OutError)) return false;
				if (!Pairs.IsValid() || Pairs->GetKind() != EUeAgentValueKind::Array || Pairs->GetArrayItems().IsEmpty())
				{
					return ReadFailure(OutError, TEXT("modeling_geometry_weld_parameters_invalid"),
						TEXT("Welding requires a non-empty array of edge pairs."), TEXT("/after/typed_parameters/edge_pairs"),
						Pairs, TEXT("non-empty array of edge pairs"));
				}
				const TArray<FUeAgentCanonicalValueRef>& PairValues = Pairs->GetArrayItems();
				for (int32 PairIndex = 0; PairIndex < PairValues.Num(); ++PairIndex)
				{
					const FUeAgentCanonicalValueRef& PairValue = PairValues[PairIndex];
					if (PairValue->GetKind()
							!= EUeAgentValueKind::Array
						|| PairValue->GetArrayItems().Num() != 2)
					{
						return ReadFailure(OutError, TEXT("modeling_geometry_weld_pair_invalid"),
							TEXT("A weld pair must contain exactly two edge IDs."),
							FString::Printf(TEXT("/after/typed_parameters/edge_pairs/%d"), PairIndex),
							FUeAgentCanonicalValuePtr(PairValue), TEXT("array containing exactly two edge IDs"));
					}
					const FUeAgentCanonicalValueRef& AValue =
						PairValue->GetArrayItems()[0];
					const FUeAgentCanonicalValueRef& BValue =
						PairValue->GetArrayItems()[1];
					int32 EdgeA = INDEX_NONE;
					int32 EdgeB = INDEX_NONE;
					if (!ReadElementId(AValue, EdgeA, OutError, FString::Printf(TEXT("/after/typed_parameters/edge_pairs/%d/0"), PairIndex))
						|| !ReadElementId(BValue, EdgeB, OutError, FString::Printf(TEXT("/after/typed_parameters/edge_pairs/%d/1"), PairIndex)))
					{
						return false;
					}
					if (!Action.WorkingMesh.IsEdge(EdgeA)
						|| !Action.WorkingMesh.IsEdge(EdgeB)
						|| !Action.WorkingMesh.IsBoundaryEdge(EdgeA)
						|| !Action.WorkingMesh.IsBoundaryEdge(EdgeB))
					{
						const int32 PairElement = !Action.WorkingMesh.IsEdge(EdgeA)
							|| (Action.WorkingMesh.IsEdge(EdgeB) && !Action.WorkingMesh.IsBoundaryEdge(EdgeA)) ? 0 : 1;
						const int32 InvalidEdge = PairElement == 0 ? EdgeA : EdgeB;
						OutError = NativeFact(TEXT("modeling_geometry_weld_pair_stale"),
							Action.WorkingMesh.IsEdge(InvalidEdge) ? TEXT("The weld pair contains an edge that is not a boundary edge.")
								: TEXT("The weld pair contains an edge that no longer exists."),
							FString::Printf(TEXT("/after/typed_parameters/edge_pairs/%d/%d"), PairIndex, PairElement),
							MakeShared<FJsonValueNumber>(InvalidEdge), MakeShared<FJsonValueString>(TEXT("existing boundary edge ID")), Action.TargetRef);
						return false;
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
						OutError = NativeFact(TEXT("modeling_geometry_weld_tolerance_exceeded"),
							FString::Printf(TEXT("Edges %d and %d exceed the weld tolerance for both endpoint pairings."), EdgeA, EdgeB),
							FString::Printf(TEXT("/weld/edge_pairs/%d/endpoint_distance_cm"), PairIndex),
							NativeNumberFact(FMath::Min(DirectDistance, ReverseDistance)), NativeNumberFact(Tolerance), Action.TargetRef);
						return false;
					}
					FDynamicMesh3::FMergeEdgesInfo MergeInfo;
					const UE::Geometry::EMeshResult MergeResult = Action.WorkingMesh.MergeEdges(
						EdgeA, EdgeB, 0.5, MergeInfo, true);
					if (MergeResult != UE::Geometry::EMeshResult::Ok)
					{
						OutError = NativeFact(TEXT("modeling_geometry_weld_failed"),
							FString::Printf(TEXT("Native boundary-edge welding failed for edges %d and %d (EMeshResult)."), EdgeA, EdgeB),
							FString::Printf(TEXT("/weld/edge_pairs/%d/merge_result"), PairIndex),
							MakeShared<FJsonValueNumber>(static_cast<int32>(MergeResult)),
							MakeShared<FJsonValueNumber>(static_cast<int32>(UE::Geometry::EMeshResult::Ok)), Action.TargetRef);
						return false;
					}
				}
				return true;
			}

			if (Key == TEXT("boolean"))
			{
				FTransform OperandTransform;
				FString Operation;
				bool bSimplifyOutput = false;
				if (!ReadTransform(FindField(Action.Parameters, TEXT("operand_to_target")),
					TEXT("translation"), TEXT("rotation_quaternion_xyzw"), TEXT("scale"),
					OperandTransform, OutError, TEXT("/after/typed_parameters/operand_to_target"))
					|| !ReadString(Action.Parameters, TEXT("operation"), Operation, OutError)
					|| !ReadBoolean(Action.Parameters, TEXT("simplify_output"), bSimplifyOutput, OutError)) return false;
				FDynamicMesh3 Result;
				if (!TryComputeModelingBoolean(Action.WorkingMesh, Action.OperandMesh,
					OperandTransform, Operation, bSimplifyOutput, Result, OutError)) return false;
				Action.WorkingMesh = MoveTemp(Result);
				return true;
			}

			TStrongObjectPtr<UDynamicMesh> TargetMesh =
				MakeTransientMesh(Action.WorkingMesh, Action.OperationKey, OutError);
			if (!TargetMesh.IsValid())
			{
				return false;
			}

			if (Key == TEXT("mesh_cut")
				|| Key == TEXT("trim"))
			{
				const FUeAgentCanonicalValuePtr OperandTransformValue =
					FindField(
						Action.Parameters,
						TEXT("operand_to_target"));
				FTransform OperandTransform;
				if (!ReadTransform(
						OperandTransformValue,
						TEXT("translation"),
						TEXT("rotation_quaternion_xyzw"),
						TEXT("scale"),
						OperandTransform,
						OutError,
						TEXT("/after/typed_parameters/operand_to_target")))
				{
					return false;
				}
				TStrongObjectPtr<UDynamicMesh> OperandMesh =
					MakeTransientMesh(Action.OperandMesh, Action.OperationKey, OutError);
				if (!OperandMesh.IsValid())
				{
					return false;
				}
				FGeometryScriptMeshBooleanOptions Options;
				Options.OutputTransformSpace =
					EGeometryScriptBooleanOutputSpace::
						TargetTransformSpace;
				EGeometryScriptBooleanOperation Operation =
					EGeometryScriptBooleanOperation::Union;
				if (Key == TEXT("mesh_cut"))
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
							Options.bFillHoles,
							OutError))
					{
						return false;
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
							Options.bFillHoles,
							OutError))
					{
						return false;
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
						VoxelSize,
						OutError)
					|| !ReadNumber(
						Action.Parameters,
						TEXT("adaptivity"),
						Adaptivity,
						OutError)
					|| !ReadTransform(
						OperandTransformValue,
						TEXT("translation"),
						TEXT("rotation_quaternion_xyzw"),
						TEXT("scale"),
						OperandTransform,
						OutError,
						TEXT("/after/typed_parameters/operand_to_target")))
				{
					return false;
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
						> static_cast<double>(MAX_int32))
				{
					const TSharedRef<FJsonObject> Facts = MakeShared<FJsonObject>();
					Facts->SetField(TEXT("voxel_count"), NativeNumberFact(RequestedVoxelCount));
					Facts->SetField(TEXT("major_axis_cm"), NativeNumberFact(MajorAxisSize));
					Facts->SetField(TEXT("voxel_size_cm"), NativeNumberFact(VoxelSize));
					OutError = NativeFact(TEXT("modeling_geometry_voxel_resolution_invalid"),
						TEXT("The native voxel resolution is not representable as a finite 32-bit voxel count."),
						TEXT("/voxel_boolean/voxel_count"), MakeShared<FJsonValueObject>(Facts),
						MakeShared<FJsonValueString>(TEXT("finite voxel count no greater than 2147483647")), Action.TargetRef);
					return false;
				}
				VoxelBoolean.VoxelCount = FMath::Max(
					FMath::CeilToInt(RequestedVoxelCount),
					1);
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
					const TSharedRef<FJsonObject> Facts = MakeShared<FJsonObject>();
					Facts->SetBoolField(TEXT("result_available"), Result.IsValid());
					if (Result.IsValid()) Facts->SetField(TEXT("counts"), NativeMeshCountsFact(*Result));
					Facts->SetStringField(TEXT("operation"), RequestedOperation);
					Facts->SetField(TEXT("voxel_size_cm"), NativeNumberFact(VoxelSize));
					Facts->SetField(TEXT("adaptivity"), NativeNumberFact(Adaptivity));
					OutError = NativeFact(TEXT("modeling_geometry_voxel_boolean_failed"),
						Result.IsValid() ? TEXT("The native voxel boolean returned a mesh with no triangles.")
							: TEXT("The native voxel boolean did not return a mesh result."),
						TEXT("/voxel_boolean/result"), MakeShared<FJsonValueObject>(Facts),
						MakeShared<FJsonValueString>(TEXT("mesh result with at least one triangle")), Action.TargetRef);
					return false;
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
				if (!ReadPlane(Action.Parameters, Origin, Normal, OutError)
					|| !ReadBoolean(
						Action.Parameters,
						TEXT("keep_original"),
						bKeepOriginal,
						OutError)
					|| !ReadBoolean(
						Action.Parameters,
						TEXT("weld_seam"),
						bWeldSeam,
						OutError))
				{
					return false;
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
						MakeTransientMesh(Mirrored, Action.OperationKey, OutError);
					if (!MirroredMesh.IsValid()) return false;
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
				if (!ReadPlane(Action.Parameters, Origin, Normal, OutError)
					|| !ReadString(
						Action.Parameters,
						TEXT("keep_side"),
						KeepSide,
						OutError)
					|| !ReadBoolean(
						Action.Parameters,
						TEXT("fill_hole"),
						bFill,
						OutError))
				{
					return false;
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
							bFill,
							OutError)
						|| !CutMeshHalf(
							PositiveHalf,
							Origin,
							Normal,
							true,
							bFill,
							OutError))
					{
						return false;
					}
					TargetMesh->SetMesh(MoveTemp(NegativeHalf));
					TStrongObjectPtr<UDynamicMesh> PositiveMesh =
						MakeTransientMesh(PositiveHalf, Action.OperationKey, OutError);
					if (!PositiveMesh.IsValid())
					{
						return false;
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
							bFill,
							OutError))
					{
						return false;
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
						TargetEdgeLength,
						OutError)
					|| !ReadInteger(
						Action.Parameters,
						TEXT("iterations"),
						Iterations,
						OutError)
					|| !ReadBoolean(
						Action.Parameters,
						TEXT("preserve_boundaries"),
						bPreserveBoundaries,
						OutError)
					|| !ReadBoolean(
						Action.Parameters,
						TEXT("preserve_sharp_edges"),
						bPreserveSharpEdges,
						OutError))
				{
					return false;
				}
				bool bRemeshSucceeded = false;
				bool bRemeshVisited = false;
				TargetMesh->EditMesh(
					[&](FDynamicMesh3& EditMesh)
					{
						bRemeshVisited = true;
						bRemeshSucceeded = ApplyUniformRemeshWithPolicy(
							EditMesh,
							TargetEdgeLength,
							Iterations,
							bPreserveBoundaries,
							bPreserveSharpEdges,
							OutError);
					},
					EDynamicMeshChangeType::GeneralEdit,
					EDynamicMeshAttributeChangeFlags::Unknown,
					false);
				if (!bRemeshSucceeded)
				{
					if (!bRemeshVisited)
					{
						OutError = NativeFact(TEXT("modeling_geometry_remesh_failed"),
							TEXT("The native remesher could not edit the mesh because its callback was not invoked."),
							TEXT("/remesh/edit_callback_invoked"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					}
					return false;
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
						TargetValue,
						OutError)
					|| !ReadBoolean(
						Action.Parameters,
						TEXT("preserve_boundaries"),
						bPreserveBoundaries,
						OutError))
				{
					return false;
				}
				const bool bUseTolerance =
					TargetMode == TEXT("tolerance_cm");
				if (!bUseTolerance
					&& TargetMode != TEXT("triangle_count")
					&& TargetMode != TEXT("triangle_ratio"))
				{
					TArray<TSharedPtr<FJsonValue>> SupportedModes;
					for (const TCHAR* Supported : {TEXT("tolerance_cm"), TEXT("triangle_count"), TEXT("triangle_ratio")})
						SupportedModes.Add(MakeShared<FJsonValueString>(Supported));
					OutError = NativeFact(TEXT("modeling_geometry_simplify_target_mode_invalid"),
						TEXT("The simplification target mode is not supported."), TEXT("/after/typed_parameters/target_mode"),
						MakeShared<FJsonValueString>(TargetMode), MakeShared<FJsonValueArray>(MoveTemp(SupportedModes)), Action.TargetRef);
					return false;
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
					OutError = NativeFact(TEXT("modeling_geometry_simplify_no_effect"),
						TEXT("Native simplification did not reduce the triangle count."), TEXT("/simplify/triangle_count"),
						MakeShared<FJsonValueNumber>(SimplifiedMesh.TriangleCount()),
						MakeShared<FJsonValueNumber>(Action.WorkingMesh.TriangleCount()), Action.TargetRef);
					return false;
				}
				if (!bUseTolerance
					&& SimplifiedMesh.TriangleCount()
						> TriangleTarget + 1)
				{
					const TSharedRef<FJsonObject> Actual = MakeShared<FJsonObject>();
					Actual->SetNumberField(TEXT("triangle_count"), SimplifiedMesh.TriangleCount());
					Actual->SetBoolField(TEXT("preserve_boundaries"), bPreserveBoundaries);
					const TSharedRef<FJsonObject> Expected = MakeShared<FJsonObject>();
					Expected->SetNumberField(TEXT("target_triangle_count"), TriangleTarget);
					Expected->SetNumberField(TEXT("maximum_triangle_count"), TriangleTarget + 1);
					OutError = NativeFact(TEXT("modeling_geometry_simplify_target_unreachable"),
						TEXT("Native simplification could not reach the requested triangle target within its one-triangle tolerance."),
						TEXT("/simplify/triangle_count"), MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueObject>(Expected), Action.TargetRef);
					return false;
				}
				const FBoundarySemanticObservation AfterBoundary =
					ObserveBoundarySemantics(SimplifiedMesh);
				if (bPreserveBoundaries
					&& AfterBoundary != BeforeBoundary)
				{
					OutError = BoundaryDifferenceFailure(BeforeBoundary, AfterBoundary, Action.TargetRef);
					return false;
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
						Levels,
						OutError))
				{
					return false;
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
							Distance,
							OutError)
						|| !ReadNumber(
							Action.Parameters,
							TEXT("roundness"),
							Roundness,
							OutError)
						|| !ReadInteger(
							Action.Parameters,
							TEXT("segments"),
							Segments,
							OutError))
					{
						return false;
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
							Translation,
							OutError))
					{
						return false;
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
							Distance,
							OutError)
						|| !ReadString(
							Action.Parameters,
							TEXT("direction_mode"),
							DirectionMode,
							OutError))
					{
						return false;
					}
					if (Key == TEXT("extrude_faces")
						&& DirectionMode == TEXT("vertex_normals"))
					{
						bool bExtruded = false;
						bool bExtrudeVisited = false;
						TargetMesh->EditMesh(
							[&](FDynamicMesh3& EditMesh)
							{
								bExtrudeVisited = true;
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
								if (!bExtruded)
								{
									const TSharedRef<FJsonObject> Facts = MakeShared<FJsonObject>();
									Facts->SetNumberField(TEXT("selected_triangles"), Extruder.Triangles.Num());
									Facts->SetField(TEXT("distance_cm"), NativeNumberFact(Distance));
									Facts->SetStringField(TEXT("direction_mode"), DirectionMode);
									OutError = NativeFact(TEXT("modeling_geometry_extrude_failed"),
										TEXT("Native vertex-normal extrusion could not apply to the selected region."), TEXT("/extrude/result"),
										MakeShared<FJsonValueObject>(Facts), MakeShared<FJsonValueString>(TEXT("successful region extrusion")), Action.TargetRef);
								}
							},
							EDynamicMeshChangeType::GeneralEdit,
							EDynamicMeshAttributeChangeFlags::Unknown,
							false);
						if (!bExtruded)
						{
							if (!bExtrudeVisited)
							{
								OutError = NativeFact(TEXT("modeling_geometry_extrude_failed"),
									TEXT("Native extrusion could not edit the mesh because its callback was not invoked."),
									TEXT("/extrude/edit_callback_invoked"), MakeShared<FJsonValueBoolean>(false),
									MakeShared<FJsonValueBoolean>(true), Action.TargetRef);
							}
							return false;
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
									Direction,
									OutError))
							{
								return false;
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
							Distance,
							OutError)
						|| !ReadString(
							Action.Parameters,
							TEXT("boundary_mode"),
							BoundaryMode,
							OutError))
					{
						return false;
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
							Distance,
							OutError)
						|| !ReadString(
							Action.Parameters,
							TEXT("offset_mode"),
							OffsetMode,
							OutError))
					{
						return false;
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
					TArray<TSharedPtr<FJsonValue>> SupportedKeys;
					for (const TCHAR* Supported : {TEXT("fill_holes"), TEXT("weld_edges"), TEXT("boolean"), TEXT("mesh_cut"), TEXT("trim"),
						TEXT("voxel_boolean"), TEXT("mirror"), TEXT("plane_cut"), TEXT("remesh"), TEXT("simplify"), TEXT("subdivide"),
						TEXT("bevel_edges"), TEXT("duplicate_faces"), TEXT("extrude_faces"), TEXT("push_pull_faces"), TEXT("inset_faces"), TEXT("offset_faces")})
						SupportedKeys.Add(MakeShared<FJsonValueString>(Supported));
					OutError = NativeFact(TEXT("modeling_geometry_topology_operation_unsupported"),
						TEXT("The modeling topology operation is not supported."), TEXT("/operation_key"),
						MakeShared<FJsonValueString>(Key), MakeShared<FJsonValueArray>(MoveTemp(SupportedKeys)), Action.TargetRef);
					return false;
				}
			}

			return CopyTransientMesh(
				*TargetMesh,
				Action.WorkingMesh,
				OutError);
		}

		bool NormalizeUvLayer(
			FDynamicMesh3& Mesh,
			const int32 Channel,
			FExecutionMutationError& OutError)
		{
			if (!Mesh.HasAttributes())
			{
				OutError = NativeFact(TEXT("modeling_geometry_uv_generation_failed"),
					TEXT("The mesh has no attribute set containing UV layers."), TEXT("/mesh/attributes_available"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				return false;
			}
			if (Channel < 0 || Channel >= Mesh.Attributes()->NumUVLayers())
			{
				OutError = NativeFact(TEXT("modeling_geometry_uv_generation_failed"),
					TEXT("The requested UV layer index is outside the mesh's available UV layers."), TEXT("/mesh/uv_layer_index"),
					MakeShared<FJsonValueNumber>(Channel),
					MakeShared<FJsonValueString>(FString::Printf(TEXT("0 <= index < %d"), Mesh.Attributes()->NumUVLayers())));
				return false;
			}
			FDynamicMeshUVOverlay* Overlay =
				Mesh.Attributes()->GetUVLayer(Channel);
			if (!Overlay || Overlay->ElementCount() == 0)
			{
				OutError = NativeFact(TEXT("modeling_geometry_uv_generation_failed"),
					Overlay ? TEXT("The generated UV layer has no elements.") : TEXT("The requested UV layer has no overlay."),
					FString::Printf(TEXT("/uv_layers/%d/%s"), Channel, Overlay ? TEXT("element_count") : TEXT("overlay_available")),
					Overlay ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(0))
						: TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false)),
					Overlay ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(TEXT("greater than zero")))
						: TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(true)));
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
				const TSharedRef<FJsonObject> Facts = MakeShared<FJsonObject>();
				Facts->SetField(TEXT("minimum"), NativeNumberArrayFact({Minimum.X, Minimum.Y}));
				Facts->SetField(TEXT("maximum"), NativeNumberArrayFact({Maximum.X, Maximum.Y}));
				Facts->SetField(TEXT("range"), NativeNumberFact(Range));
				OutError = NativeFact(TEXT("modeling_geometry_uv_generation_failed"),
					TEXT("The UV coordinates are outside the unit square, but their range is too small to normalize."),
					FString::Printf(TEXT("/uv_layers/%d/range"), Channel), MakeShared<FJsonValueObject>(Facts),
					MakeShared<FJsonValueString>(FString::Printf(TEXT("range > %.9g"), static_cast<double>(UE_SMALL_NUMBER))));
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
			FDynamicMesh3& OutMesh,
			FExecutionMutationError& OutError)
		{
			const FMeshDescription* Description =
				StaticMesh.GetMeshDescription(0);
			if (!Description)
			{
				OutError = NativeFact(TEXT("modeling_geometry_static_carrier_invalid"),
					TEXT("The static mesh has no LOD 0 mesh description to convert."),
					TEXT("/lods/0/mesh_description_available"), MakeShared<FJsonValueBoolean>(false),
					MakeShared<FJsonValueBoolean>(true), StaticMesh.GetPathName());
				return false;
			}
			FMeshDescriptionToDynamicMesh Converter;
			Converter.Convert(Description, OutMesh);
			return true;
		}

		bool TryCopyResolvedCarrierMesh(
			UObject& Object,
			UWorld& World,
			FDynamicMesh3& OutMesh,
			FExecutionMutationError& OutError)
		{
			if (ADynamicMeshActor* Actor = Cast<ADynamicMeshActor>(&Object))
			{
				UDynamicMeshComponent* Component = Actor->GetDynamicMeshComponent();
				UDynamicMesh* DynamicMesh = Component ? Component->GetDynamicMesh() : nullptr;
				if (Actor->GetWorld() != &World)
				{
					OutError = NativeFact(TEXT("modeling_geometry_operand_carrier_invalid"),
						TEXT("The dynamic mesh operand belongs to a different world."),
						TEXT("/carrier/world"), Actor->GetWorld()
							? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Actor->GetWorld()->GetPathName()))
							: TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()),
						MakeShared<FJsonValueString>(World.GetPathName()), Actor->GetPathName());
					return false;
				}
				if (!DynamicMesh)
				{
					OutError = NativeFact(TEXT("modeling_geometry_operand_carrier_invalid"),
						Component ? TEXT("The dynamic mesh operand component has no mesh.")
							: TEXT("The dynamic mesh operand has no dynamic mesh component."),
						Component ? TEXT("/carrier/dynamic_mesh_available") : TEXT("/carrier/component_available"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Actor->GetPathName());
					return false;
				}
				if (!CopyTransientMesh(*DynamicMesh, OutMesh, OutError))
				{
					SupplementNativeTarget(OutError, Actor->GetPathName());
					return false;
				}
				return true;
			}
			if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(&Object))
			{
				if (!ConvertStaticMesh(*StaticMesh, OutMesh, OutError)) return false;
				if (OutMesh.VertexCount() <= 0 || OutMesh.TriangleCount() <= 0)
				{
					OutError = NativeFact(TEXT("modeling_geometry_operand_carrier_invalid"),
						TEXT("The converted static mesh operand is empty."), TEXT("/carrier/mesh_counts"),
						NativeMeshCountsFact(OutMesh), MakeShared<FJsonValueString>(TEXT("positive vertex and triangle counts")),
						StaticMesh->GetPathName());
					return false;
				}
				return true;
			}
			TArray<TSharedPtr<FJsonValue>> SupportedClasses;
			SupportedClasses.Add(MakeShared<FJsonValueString>(ADynamicMeshActor::StaticClass()->GetPathName()));
			SupportedClasses.Add(MakeShared<FJsonValueString>(UStaticMesh::StaticClass()->GetPathName()));
			OutError = NativeFact(TEXT("modeling_geometry_operand_carrier_invalid"),
				TEXT("The resolved operand is not a supported modeling mesh carrier."), TEXT("/carrier/class"),
				MakeShared<FJsonValueString>(Object.GetClass()->GetPathName()), MakeShared<FJsonValueArray>(MoveTemp(SupportedClasses)));
			OutError.InternalContext = Object.GetPathName();
			return false;
		}

		bool CommitStaticMesh(
			UStaticMesh& StaticMesh,
			const FDynamicMesh3& Mesh,
			FExecutionMutationError& OutError)
		{
			FMeshDescription Description;
			FStaticMeshAttributes Attributes(Description);
			Attributes.Register();
			FDynamicMeshToMeshDescription Converter;
			Converter.Convert(&Mesh, Description);
			StaticMesh.CreateMeshDescription(0, Description);
			StaticMesh.CommitMeshDescription(0);
			StaticMesh.PostEditChange();
			if (!StaticMesh.GetMeshDescription(0))
			{
				OutError = NativeFact(TEXT("modeling_geometry_static_commit_failed"),
					TEXT("The committed static mesh has no LOD 0 mesh description."), TEXT("/lods/0/mesh_description_available"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), StaticMesh.GetPathName());
				return false;
			}
			return true;
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
			if (Change.GetIntent() != EChangeIntent::Update)
			{
				OutError = NativeFact(TEXT("modeling_geometry_change_intent_invalid"),
					TEXT("An existing mesh carrier edit requires EChangeIntent::Update."), TEXT("/change/intent"),
					MakeShared<FJsonValueNumber>(static_cast<int32>(Change.GetIntent())),
					MakeShared<FJsonValueNumber>(static_cast<int32>(EChangeIntent::Update)));
				return false;
			}
			if (!ReadString(
					After,
					TEXT("operation_key"),
					OutAction.OperationKey,
					OutError,
					TEXT("/after"))
				|| !ReadString(
					After,
					TEXT("target_ref"),
					OutAction.TargetRef,
					OutError,
					TEXT("/after"))
				|| !ReadString(
					After,
					TEXT("carrier_kind"),
					CarrierKind,
					OutError,
					TEXT("/after"))
				|| !ReadString(
					After,
					TEXT("expected_before_fingerprint"),
					ExpectedFingerprint,
					OutError,
					TEXT("/after"),
					true))
			{
				return false;
			}
			OutAction.Parameters =
				FindField(After, TEXT("typed_parameters"));
			if (!OutAction.Parameters.IsValid())
			{
				return ReadFailure(OutError, TEXT("modeling_geometry_parameters_missing"),
					TEXT("The planned mesh edit has no typed parameters."), TEXT("/after/typed_parameters"),
					OutAction.Parameters, TEXT("typed modeling parameters"));
			}
			UObject* Object = nullptr;
			if (!Resolver.TryResolveReference(
					World,
					OutAction.TargetRef,
					Object,
					OutError)) return false;
			if (!Object)
			{
				OutError = NativeFact(TEXT("modeling_geometry_carrier_unavailable"),
					TEXT("The target reference resolved without a mesh carrier object."), TEXT("/carrier/object_available"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), OutAction.TargetRef);
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
					if (!Actor)
					{
						OutError = NativeFact(TEXT("modeling_geometry_dynamic_carrier_invalid"),
							TEXT("The resolved dynamic mesh carrier is not an ADynamicMeshActor."), TEXT("/carrier/class"),
							MakeShared<FJsonValueString>(Object->GetClass()->GetPathName()),
							MakeShared<FJsonValueString>(ADynamicMeshActor::StaticClass()->GetPathName()), OutAction.TargetRef);
					}
					else if (Actor->GetWorld() != &World)
					{
						OutError = NativeFact(TEXT("modeling_geometry_dynamic_carrier_invalid"),
							TEXT("The dynamic mesh carrier belongs to a different world."), TEXT("/carrier/world"),
							Actor->GetWorld() ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Actor->GetWorld()->GetPathName()))
								: TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()),
							MakeShared<FJsonValueString>(World.GetPathName()), OutAction.TargetRef);
					}
					else
					{
						OutError = NativeFact(TEXT("modeling_geometry_dynamic_carrier_invalid"),
							Component ? TEXT("The dynamic mesh carrier component has no mesh.") : TEXT("The dynamic mesh carrier has no mesh component."),
							Component ? TEXT("/carrier/dynamic_mesh_available") : TEXT("/carrier/component_available"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), OutAction.TargetRef);
					}
					return false;
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
				if (!StaticMesh)
				{
					OutError = NativeFact(TEXT("modeling_geometry_static_carrier_invalid"),
						TEXT("The resolved static mesh carrier is not a UStaticMesh."), TEXT("/carrier/class"),
						Object ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Object->GetClass()->GetPathName()))
							: TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()),
						MakeShared<FJsonValueString>(UStaticMesh::StaticClass()->GetPathName()), OutAction.TargetRef);
					return false;
				}
				if (!ConvertStaticMesh(*StaticMesh, OutAction.OriginalMesh, OutError)) return false;
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
				TArray<TSharedPtr<FJsonValue>> SupportedKinds;
				SupportedKinds.Add(MakeShared<FJsonValueString>(TEXT("dynamic_mesh_actor")));
				SupportedKinds.Add(MakeShared<FJsonValueString>(TEXT("static_mesh_asset")));
				OutError = NativeFact(TEXT("modeling_geometry_carrier_unsupported"),
					TEXT("The modeling mesh carrier kind is not supported."), TEXT("/after/carrier_kind"),
					MakeShared<FJsonValueString>(CarrierKind), MakeShared<FJsonValueArray>(MoveTemp(SupportedKinds)), OutAction.TargetRef);
				return false;
			}
			const FString ActualFingerprint = MeshFingerprint(OutAction.OriginalMesh);
			if (ActualFingerprint != ExpectedFingerprint)
			{
				OutError = NativeFact(TEXT("modeling_geometry_checkout_fingerprint_mismatch"),
					TEXT("The carrier mesh changed after the planned input was captured."), TEXT("/carrier/matches_planned_mesh"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), OutAction.TargetRef);
				OutError.ActualEvidence = MakeShared<FJsonValueString>(ActualFingerprint);
				OutError.ExpectedEvidence = MakeShared<FJsonValueString>(ExpectedFingerprint);
				return false;
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
				if (!OperandObject)
				{
					OutError = NativeFact(TEXT("modeling_geometry_operand_carrier_invalid"),
						TEXT("The operand reference resolved without a mesh carrier object."), TEXT("/operand/object_available"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), OutAction.OperandRef);
					return false;
				}
				if (!TryCopyResolvedCarrierMesh(*OperandObject, World, OutAction.OperandMesh, OutError)) return false;
				FString ExpectedOperandFingerprint;
				if (!ReadString(
						After,
						TEXT("operand_before_fingerprint"),
						ExpectedOperandFingerprint,
						OutError,
						TEXT("/after"),
						true)) return false;
				const FString ActualOperandFingerprint = MeshFingerprint(OutAction.OperandMesh);
				if (ActualOperandFingerprint != ExpectedOperandFingerprint)
				{
					OutError = NativeFact(TEXT("modeling_geometry_operand_fingerprint_mismatch"),
						TEXT("The operand mesh changed after the planned input was captured."), TEXT("/operand/matches_planned_mesh"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), OutAction.OperandRef);
					OutError.ActualEvidence = MakeShared<FJsonValueString>(ActualOperandFingerprint);
					OutError.ExpectedEvidence = MakeShared<FJsonValueString>(ExpectedOperandFingerprint);
					return false;
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
			if (Change.GetIntent() != EChangeIntent::Create)
			{
				OutError = NativeFact(TEXT("modeling_geometry_change_intent_invalid"),
					TEXT("A primitive output requires EChangeIntent::Create."), TEXT("/change/intent"),
					MakeShared<FJsonValueNumber>(static_cast<int32>(Change.GetIntent())),
					MakeShared<FJsonValueNumber>(static_cast<int32>(EChangeIntent::Create)));
				return false;
			}
			if (!ReadString(
					After,
					TEXT("operation_key"),
					OutAction.OperationKey,
					OutError,
					TEXT("/after"))
				|| !ReadString(
					After,
					TEXT("target_ref"),
					OutAction.TargetRef,
					OutError,
					TEXT("/after")))
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
			if (!ReadString(
					Placement,
					TEXT("unit"),
					Unit,
					OutError,
					TEXT("/after/typed_parameters/placement"))
				|| !ReadNumberArray(
					Placement,
					TEXT("translation"),
					3,
					Translation,
					OutError,
					TEXT("/after/typed_parameters/placement"))
				|| !ReadNumberArray(
					Placement,
					TEXT("rotation_quaternion_xyzw"),
					4,
					Rotation,
					OutError,
					TEXT("/after/typed_parameters/placement"))
				|| !ReadNumberArray(
					Placement,
					TEXT("scale"),
					3,
					Scale,
					OutError,
					TEXT("/after/typed_parameters/placement")))
			{
				return false;
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
				OutError = NativeFact(TEXT("modeling_geometry_placement_invalid"),
					TEXT("The primitive placement quaternion is too small to define a rotation."),
					TEXT("/after/typed_parameters/placement/rotation_quaternion_xyzw"), NativeNumberArrayFact(Rotation),
					MakeShared<FJsonValueString>(FString::Printf(TEXT("quaternion squared length > %.17g"), UE_DOUBLE_SMALL_NUMBER)), OutAction.TargetRef);
				return false;
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
			FString MaterialRef = UE::UAI::ModelingMetricMaterial::AssetPath();
			if (FindField(OutAction.Parameters, TEXT("material_ref")).IsValid()
				&& !ReadString(OutAction.Parameters, TEXT("material_ref"), MaterialRef, OutError)) return false;
			OutAction.PrimitiveMaterial.Reset(LoadObject<UMaterialInterface>(nullptr, *MaterialRef));
			if (!OutAction.PrimitiveMaterial.IsValid())
			{
				OutError = NativeFact(TEXT("modeling_material_not_found"), TEXT("The modeling material could not be loaded. Install the plugin Content directory with the plugin."),
					TEXT("/after/typed_parameters/material_ref"), MakeShared<FJsonValueString>(MaterialRef), MakeShared<FJsonValueString>(TEXT("loadable MaterialInterface")), OutAction.TargetRef);
				return false;
			}
			if (FindField(OutAction.Parameters, TEXT("color")).IsValid())
			{
				TArray<double> RGB;
				if (!ReadNumberArray(OutAction.Parameters, TEXT("color"), 3, RGB, OutError)) return false;
				if (!UE::UAI::ModelingMetricMaterial::IsMetric(OutAction.PrimitiveMaterial.Get()))
				{
					OutError = NativeFact(TEXT("modeling_color_requires_metric_material"), TEXT("color applies to the built-in metric material; edit a custom material through its own parameters."),
						TEXT("/after/typed_parameters/material_ref"), MakeShared<FJsonValueString>(MaterialRef), MakeShared<FJsonValueString>(UE::UAI::ModelingMetricMaterial::AssetPath()), OutAction.TargetRef);
					return false;
				}
				OutAction.PrimitiveColor = FLinearColor(RGB[0], RGB[1], RGB[2], 1.0f);
			}
			OutAction.CanonicalKey =
				Change.GetIdentity().GetCanonicalKey();
			return true;
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
						Channel,
						OutError))
				{
					return false;
				}
				TStrongObjectPtr<UDynamicMesh> DynamicMesh =
					MakeTransientMesh(Action.WorkingMesh, Action.OperationKey, OutError);
				if (!DynamicMesh.IsValid())
				{
					return false;
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
							bPack,
							OutError)
						|| !ReadInteger(
							Action.Parameters,
							TEXT("texture_resolution"),
							TextureResolution,
							OutError))
					{
						return false;
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
							Dimensions,
							OutError)
						|| !ReadNumberArray(
							Frame,
							TEXT("origin"),
							3,
							Origin,
							OutError,
							TEXT("/after/typed_parameters/projection_frame"))
						|| !ReadNumberArray(
							Frame,
							TEXT("axis_u"),
							3,
							AxisU,
							OutError,
							TEXT("/after/typed_parameters/projection_frame"))
						|| !ReadNumberArray(
							Frame,
							TEXT("axis_v"),
							3,
							AxisV,
							OutError,
							TEXT("/after/typed_parameters/projection_frame"))
						|| !ReadNumberArray(
							Frame,
							TEXT("axis_w"),
							3,
							AxisW,
							OutError,
							TEXT("/after/typed_parameters/projection_frame")))
					{
						return false;
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
						|| FMath::Abs(FVector::DotProduct(U, V)) > 0.001f
						|| FVector::DotProduct(FVector::CrossProduct(U, V), W) < 0.999f)
					{
						if (!U.IsNormalized() || !V.IsNormalized() || !W.IsNormalized())
						{
							const TCHAR* AxisName = !U.IsNormalized() ? TEXT("axis_u") : !V.IsNormalized() ? TEXT("axis_v") : TEXT("axis_w");
							const TArray<double>& Axis = !U.IsNormalized() ? AxisU : !V.IsNormalized() ? AxisV : AxisW;
							OutError = NativeFact(TEXT("modeling_geometry_projection_basis_invalid"),
								TEXT("A projection frame axis must have unit length."),
								NativeFieldPath(TEXT("/after/typed_parameters/projection_frame"), AxisName),
								NativeNumberArrayFact(Axis), MakeShared<FJsonValueString>(TEXT("normalized direction vector")), Action.TargetRef);
						}
						else if (FMath::Abs(FVector::DotProduct(U, V)) > 0.001f)
						{
							OutError = NativeFact(TEXT("modeling_geometry_projection_basis_invalid"),
								TEXT("Projection frame axes U and V are not perpendicular within the allowed absolute dot product."),
								TEXT("/projection_frame/uv_absolute_dot_product"), NativeNumberFact(FMath::Abs(FVector::DotProduct(U, V))),
								NativeNumberFact(0.001f), Action.TargetRef);
						}
						else
						{
							OutError = NativeFact(TEXT("modeling_geometry_projection_basis_invalid"),
								TEXT("Projection frame axis W does not align with the right-handed U/V frame within the allowed dot product."),
								TEXT("/projection_frame/w_alignment"), NativeNumberFact(FVector::DotProduct(FVector::CrossProduct(U, V), W)),
								NativeNumberFact(0.999f), Action.TargetRef);
						}
						return false;
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
						Channel,
						OutError))
				{
					return false;
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
						bPreserveHardEdges,
						OutError))
				{
					return false;
				}
				TStrongObjectPtr<UDynamicMesh> DynamicMesh =
					MakeTransientMesh(Action.WorkingMesh, Action.OperationKey, OutError);
				if (!DynamicMesh.IsValid())
				{
					return false;
				}
				if (Mode == TEXT("face"))
				{
					UGeometryScriptLibrary_MeshNormalsFunctions::
						SetPerFaceNormals(DynamicMesh.Get());
				}
				else if (Mode == TEXT("angle_threshold"))
				{
					double Angle = 30.0;
					ReadNumberProbe(
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
						SlotIndex,
						OutError)
					|| !ReadElementIds(
						Selection,
						TEXT("element_ids"),
						TriangleIds,
						OutError,
						TEXT("/after/typed_parameters/selection")))
				{
					return false;
				}
				Action.WorkingMesh.EnableAttributes();
				FDynamicMeshAttributeSet* Attributes =
					Action.WorkingMesh.Attributes();
				Attributes->EnableMaterialID();
				FDynamicMeshMaterialAttribute* MaterialIds =
					Attributes->GetMaterialID();
				for (int32 SelectionIndex = 0; SelectionIndex < TriangleIds.Num(); ++SelectionIndex)
				{
					const int32 TriangleId = TriangleIds[SelectionIndex];
					if (!Action.WorkingMesh.IsTriangle(TriangleId))
					{
						OutError = NativeFact(TEXT("modeling_geometry_material_selection_stale"),
							TEXT("The material selection contains a triangle that no longer exists."),
							FString::Printf(TEXT("/after/typed_parameters/selection/element_ids/%d"), SelectionIndex),
							MakeShared<FJsonValueNumber>(TriangleId), MakeShared<FJsonValueString>(TEXT("existing triangle ID")), Action.TargetRef);
						return false;
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
					OutError = NativeFact(TEXT("modeling_geometry_material_reference_invalid"),
						TEXT("The resolved material reference is not a material interface."), TEXT("/after/typed_parameters/material_ref"),
						MaterialObject ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(MaterialObject->GetClass()->GetPathName()))
							: TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()),
						MakeShared<FJsonValueString>(UMaterialInterface::StaticClass()->GetPathName()), MaterialRef);
					return false;
				}
				if (Action.Carrier
					!= ECarrierKind::StaticMeshAsset)
				{
					OutError = NativeFact(TEXT("modeling_geometry_named_slot_carrier_unsupported"),
						TEXT("Named material slots require a static mesh asset carrier."), TEXT("/carrier/kind"),
						MakeShared<FJsonValueString>(Action.Carrier == ECarrierKind::PrimitiveOutput ? TEXT("primitive_output") : TEXT("dynamic_mesh_actor")),
						MakeShared<FJsonValueString>(TEXT("static_mesh_asset")), Action.TargetRef);
					return false;
				}
				Action.bMutationStarted = true;
				Action.StaticMesh->GetStaticMaterials().Add(
					FStaticMaterial(
						Material,
						FName(*SlotName),
						FName(*SlotName)));
				return true;
			}
			TArray<TSharedPtr<FJsonValue>> SupportedKeys;
			for (const TCHAR* Supported : {TEXT("auto_uv"), TEXT("project_uv"), TEXT("recompute_normals"), TEXT("assign_material_slot"), TEXT("add_material_slot")})
				SupportedKeys.Add(MakeShared<FJsonValueString>(Supported));
			OutError = NativeFact(TEXT("modeling_geometry_attribute_operation_unsupported"),
				TEXT("The modeling attribute operation is not supported."), TEXT("/operation_key"),
				MakeShared<FJsonValueString>(Action.OperationKey), MakeShared<FJsonValueArray>(MoveTemp(SupportedKeys)), Action.TargetRef);
			return false;
		}

		bool ApplyCollisionOperation(
			FModelingGeometryAction& Action,
			FExecutionMutationError& OutError)
		{
			bool bReplace = true;
			if (!ReadBoolean(
					Action.Parameters,
					TEXT("replace_existing"),
					bReplace,
					OutError))
			{
				return false;
			}
			TStrongObjectPtr<UDynamicMesh> SourceMesh =
				MakeTransientMesh(Action.WorkingMesh, Action.OperationKey, OutError);
			if (!SourceMesh.IsValid())
			{
				return false;
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
						MaxHulls,
						OutError)
					|| !ReadInteger(
						Action.Parameters,
						TEXT("max_vertices_per_hull"),
						MaxVertices,
						OutError))
				{
					return false;
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
						MaxShapes,
						OutError))
				{
					return false;
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
				TArray<TSharedPtr<FJsonValue>> SupportedKeys;
				SupportedKeys.Add(MakeShared<FJsonValueString>(TEXT("generate_convex_collision")));
				SupportedKeys.Add(MakeShared<FJsonValueString>(TEXT("generate_simple_collision")));
				OutError = NativeFact(TEXT("modeling_geometry_collision_operation_unsupported"),
					TEXT("The modeling collision operation is not supported."), TEXT("/operation_key"),
					MakeShared<FJsonValueString>(Action.OperationKey), MakeShared<FJsonValueArray>(MoveTemp(SupportedKeys)), Action.TargetRef);
				return false;
			}
			FGeometryScriptSimpleCollision Generated =
				UGeometryScriptLibrary_CollisionFunctions::
					GenerateCollisionFromMesh(
						SourceMesh.Get(),
						Options);
			if (Generated.AggGeom.GetElementCount() == 0)
			{
				const TSharedRef<FJsonObject> Facts = MakeShared<FJsonObject>();
				Facts->SetNumberField(TEXT("generated_shape_count"), 0);
				Facts->SetStringField(TEXT("operation"), Action.OperationKey);
				Facts->SetStringField(TEXT("method_type"), TEXT("EGeometryScriptCollisionGenerationMethod"));
				Facts->SetNumberField(TEXT("method_value"), static_cast<int32>(Options.Method));
				Facts->SetNumberField(TEXT("maximum_shape_count"), Options.MaxShapeCount);
				Facts->SetField(TEXT("mesh_counts"), NativeMeshCountsFact(Action.WorkingMesh));
				OutError = NativeFact(TEXT("modeling_geometry_collision_generation_failed"),
					TEXT("Native collision generation produced no collision shapes."), TEXT("/collision/generated_shapes"),
					MakeShared<FJsonValueObject>(Facts), MakeShared<FJsonValueString>(TEXT("at least one collision shape")), Action.TargetRef);
				return false;
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
			Action.bMutationStarted = true;
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
					OutError = NativeFact(TEXT("modeling_geometry_primitive_spawn_failed"),
						TEXT("The editor could not spawn the requested dynamic mesh primitive actor."), TEXT("/primitive/actor_available"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Action.TargetRef);
					return false;
				}
				Action.CreatedActor = Actor;
				Action.bMutationStarted = true;
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
					OutError = NativeFact(TEXT("modeling_geometry_primitive_mesh_missing"),
						Component ? TEXT("The spawned primitive component has no dynamic mesh.")
							: TEXT("The spawned primitive actor has no dynamic mesh component."),
						Component ? TEXT("/primitive/dynamic_mesh_available") : TEXT("/primitive/component_available"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Action.TargetRef);
					return false;
				}
				DynamicMesh->EditMesh(
					[&Action](FDynamicMesh3& Mesh)
					{
						Mesh = Action.WorkingMesh;
					});
				Component->SetMaterial(0, Action.PrimitiveMaterial.Get());
				UE::UAI::ModelingMetricMaterial::Initialize(*Component, FBox(Action.WorkingMesh.GetBounds().Min, Action.WorkingMesh.GetBounds().Max), Action.PrimitiveColor);
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
					Action.bMutationStarted = true;
					Component->Modify();
					Component->GetDynamicMesh()->EditMesh(
						[&Action](FDynamicMesh3& Mesh)
						{
							Mesh = Action.WorkingMesh;
						});
					Component->NotifyMeshUpdated();
				}
				else
				{
					Action.bMutationStarted = true;
					if (!CommitStaticMesh(*Action.StaticMesh, Action.WorkingMesh, OutError)) return false;
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
			FModelingGeometryAction& Action,
			FExecutionMutationError& OutError)
		{
			const auto FailRollback = [&OutError]()
			{
				OutError.Stage = TEXT("rollback");
				OutError.RollbackAttempted = true;
				OutError.RollbackSucceeded = false;
				return false;
			};
			if (Action.Carrier == ECarrierKind::PrimitiveOutput)
			{
				ADynamicMeshActor* Actor = Action.CreatedActor.Get();
				const bool bDestroyed = !Actor
					|| (Actor->GetWorld() == &World
						&& World.DestroyActor(Actor));
				if (!bDestroyed)
				{
					const bool bWrongWorld = Actor->GetWorld() != &World;
					OutError = NativeFact(TEXT("modeling_geometry_primitive_rollback_failed"),
						bWrongWorld ? TEXT("The created primitive actor belongs to another world and could not be removed during rollback.")
							: TEXT("The editor could not destroy the created primitive actor during rollback."),
						bWrongWorld ? TEXT("/rollback/primitive/world_matches") : TEXT("/rollback/primitive/destroyed"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Action.TargetRef);
					return FailRollback();
				}
				if (Action.Package.IsValid())
				{
					Action.Package->SetDirtyFlag(
						Action.bPackageWasDirty);
				}
				return true;
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
					const TCHAR* Field = !Actor ? TEXT("/rollback/carrier/actor_available")
						: Actor->GetWorld() != &World ? TEXT("/rollback/carrier/world_matches")
						: !Component ? TEXT("/rollback/carrier/component_available") : TEXT("/rollback/carrier/dynamic_mesh_available");
					const TCHAR* Reason = !Actor ? TEXT("The dynamic mesh actor is unavailable during rollback.")
						: Actor->GetWorld() != &World ? TEXT("The dynamic mesh actor belongs to another world during rollback.")
						: !Component ? TEXT("The dynamic mesh component is unavailable during rollback.")
						: TEXT("The dynamic mesh component has no mesh to restore during rollback.");
					OutError = NativeFact(TEXT("modeling_geometry_dynamic_rollback_failed"), Reason, Field,
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Action.TargetRef);
					return FailRollback();
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
				if (!StaticMesh)
				{
					OutError = NativeFact(TEXT("modeling_geometry_static_rollback_failed"),
						TEXT("The static mesh asset is unavailable during rollback."), TEXT("/rollback/carrier/static_mesh_available"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Action.TargetRef);
					return FailRollback();
				}
				if (!CommitStaticMesh(*StaticMesh, Action.OriginalMesh, OutError)) return FailRollback();
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
			TArray<FModelingGeometryAction>& Actions,
			FExecutionMutationError& OutError)
		{
			OutError = FExecutionMutationError();
			bool bConfirmed = true;
			for (int32 Index = Actions.Num() - 1;
				Index >= 0;
				--Index)
			{
				FExecutionMutationError ActionError;
				if (!RollbackAction(World, Actions[Index], ActionError))
				{
					if (bConfirmed)
					{
						OutError = NativeFact(TEXT("modeling_geometry_rollback_unconfirmed"),
							TEXT("One or more modeling changes could not be restored."), TEXT("/rollback/confirmed"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
						OutError.Stage = TEXT("rollback");
						OutError.RollbackAttempted = true;
						OutError.RollbackSucceeded = false;
					}
					OutError.Diagnostics.Add(MoveTemp(ActionError));
					bConfirmed = false;
				}
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
				OutResult = FUeAgentGenericEffectApplyResult::NoMutation();
				OutError = FExecutionMutationError();
				if (!IsInGameThread())
				{
					OutError = NativeFact(TEXT("modeling_geometry_game_thread_required"),
						TEXT("Modeling geometry mutations must execute on the game thread."), TEXT("/execution/on_game_thread"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return false;
				}
				if (!Request.IsValid())
				{
					OutError = FExecutionMutationError::WrapFailure(TEXT("modeling_geometry_execution_request_invalid"),
						Request.GetValidationDiagnostic(), TEXT("execution"), TEXT("The modeling execution request is invalid."));
					return false;
				}
				if (!Lease.IsValid())
				{
					OutError = FExecutionMutationError::WrapFailure(TEXT("modeling_geometry_execution_lease_invalid"),
						Lease.GetValidationDiagnostic(), TEXT("execution"), TEXT("The modeling execution lease is invalid."));
					return false;
				}
				if (Request.GetExecutionContext().GetWorldIdentity() != World.GetPathName())
				{
					OutError = NativeFact(TEXT("modeling_geometry_exact_world_context_mismatch"),
						TEXT("The modeling execution world differs from the requested world."), TEXT("/execution/world"),
						MakeShared<FJsonValueString>(World.GetPathName()),
						MakeShared<FJsonValueString>(Request.GetExecutionContext().GetWorldIdentity()));
					return false;
				}
				const TArray<FPlannedChange>& Changes =
					Request.GetPlan()
						->GetChangeSet()
						.GetChanges();
				if (Changes.IsEmpty()
					|| Bindings.Num() != Changes.Num())
				{
					const TSharedRef<FJsonObject> Counts = MakeShared<FJsonObject>();
					Counts->SetNumberField(TEXT("changes"), Changes.Num());
					Counts->SetNumberField(TEXT("bindings"), Bindings.Num());
					OutError = NativeFact(TEXT("modeling_geometry_change_binding_mismatch"),
						Changes.IsEmpty() ? TEXT("The modeling execution plan contains no changes.")
							: TEXT("The modeling binding count does not match the planned change count."),
						TEXT("/execution/change_bindings"), MakeShared<FJsonValueObject>(Counts),
						MakeShared<FJsonValueString>(TEXT("non-empty changes with exactly one binding per change")));
					return false;
				}

				TArray<FModelingGeometryAction> Actions;
				Actions.Reserve(Changes.Num());
				for (const FPlannedChange& Change : Changes)
				{
					FModelingGeometryAction Action;
					const FString& TargetKind =
						Change.GetIdentity().GetTargetKind();
					bool bPrepared = false;
					if (TargetKind == TEXT("geometry.primitive.create"))
					{
						bPrepared = PreparePrimitive(Change, Action, OutError);
					}
					else if (TargetKind == TEXT("geometry.topology.edit") || TargetKind == TEXT("geometry.attribute.edit")
						|| TargetKind == TEXT("geometry.collision.edit"))
					{
						bPrepared = CheckoutExistingCarrier(World, *Resolver, Change, Action, OutError);
					}
					else
					{
						TArray<TSharedPtr<FJsonValue>> SupportedKinds;
						for (const TCHAR* Supported : {TEXT("geometry.primitive.create"), TEXT("geometry.topology.edit"),
							TEXT("geometry.attribute.edit"), TEXT("geometry.collision.edit")})
							SupportedKinds.Add(MakeShared<FJsonValueString>(Supported));
						OutError = NativeFact(TEXT("modeling_geometry_target_kind_unsupported"),
							TEXT("The planned modeling geometry target kind is not supported."), TEXT("/change/target_kind"),
							MakeShared<FJsonValueString>(TargetKind), MakeShared<FJsonValueArray>(MoveTemp(SupportedKinds)));
					}
					if (!bPrepared)
					{
						SupplementNativeTarget(OutError, Action.TargetRef);
						return false;
					}
					Actions.Add(MoveTemp(Action));
				}

				TArray<FModelingGeometryAction> AppliedActions;
				AppliedActions.Reserve(Actions.Num());
				TArray<FString> MutatedKeys;
				bool bAllApplied = true;
				for (FModelingGeometryAction& Action : Actions)
				{
					if (!CommitAction(
							Action,
							*Resolver,
							World,
							OutError))
					{
						SupplementNativeTarget(OutError, Action.TargetRef);
						bAllApplied = false;
						if (Action.bMutationStarted)
						{
							MutatedKeys.Add(Action.CanonicalKey);
							AppliedActions.Add(MoveTemp(Action));
						}
						break;
					}
					MutatedKeys.Add(Action.CanonicalKey);
					AppliedActions.Add(MoveTemp(Action));
				}

				// The shared execution owner performs rollback, including partial failures.
				if (!bAllApplied && AppliedActions.IsEmpty()) return false;
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
				if (!OutResult.IsValid())
				{
					OutError.Append(FExecutionMutationError::WrapFailure(TEXT("modeling_geometry_apply_result_invalid"),
						OutResult.GetValidationDiagnostic(), TEXT("execution"), TEXT("The modeling apply result is invalid.")));
					return false;
				}
				return bAllApplied;
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
				if (!IsInGameThread())
				{
					OutError = NativeFact(TEXT("modeling_geometry_rollback_request_invalid"),
						TEXT("Modeling geometry rollback must execute on the game thread."), TEXT("/rollback/on_game_thread"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					OutError.Stage = TEXT("rollback");
					return false;
				}
				if (!Applied.IsValid())
				{
					OutError = FExecutionMutationError::WrapFailure(TEXT("modeling_geometry_rollback_request_invalid"),
						Applied.GetValidationDiagnostic(), TEXT("rollback"), TEXT("The modeling rollback result record is invalid."));
					OutError.Stage = TEXT("rollback");
					return false;
				}
				if (Request.GetExecutionContext().GetWorldIdentity() != World.GetPathName())
				{
					OutError = NativeFact(TEXT("modeling_geometry_rollback_request_invalid"),
						TEXT("The modeling rollback world differs from the requested world."), TEXT("/rollback/world"),
						MakeShared<FJsonValueString>(World.GetPathName()),
						MakeShared<FJsonValueString>(Request.GetExecutionContext().GetWorldIdentity()));
					OutError.Stage = TEXT("rollback");
					return false;
				}
				TArray<FModelingGeometryAction> Actions;
				{
					FScopeLock Lock(&StateMutex);
					const TArray<FModelingGeometryAction>* Found =
						RollbackByToken.Find(
							Applied.GetRollbackToken());
					if (!Found)
					{
						OutError = NativeFact(TEXT("modeling_geometry_rollback_token_unknown"),
							TEXT("The modeling rollback record is unavailable."), TEXT("/rollback/record_available"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
						OutError.Stage = TEXT("rollback");
						OutError.SubjectId = Applied.GetRollbackToken();
						return false;
					}
					Actions = *Found;
				}
				bOutConfirmed = RollbackAll(World, Actions, OutError);
				if (!bOutConfirmed) return false;
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
					OutError = NativeFact(TEXT("modeling_geometry_barrier_request_invalid"),
						TEXT("The barrier request has a negative internal subject generation."), TEXT("/barrier/subject_generation_valid"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					OutError.ActualEvidence = MakeShared<FJsonValueString>(FString::Printf(TEXT("%lld"), Request.GetSubjectGeneration()));
					return false;
				}
				OutResult = FBarrierResult(
					Request.GetSubjectGeneration(),
					Request.GetRequiredSignals(),
					FString::Printf(
						TEXT("modeling-geometry:%lld"),
						Request.GetSubjectGeneration()),
					true);
				if (!OutResult.IsValid())
				{
					OutError = FExecutionMutationError::WrapFailure(TEXT("modeling_geometry_barrier_result_invalid"),
						OutResult.GetValidationDiagnostic(), TEXT("execution"), TEXT("The modeling settle result is invalid."));
					return false;
				}
				return true;
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
		return true;
	}
}

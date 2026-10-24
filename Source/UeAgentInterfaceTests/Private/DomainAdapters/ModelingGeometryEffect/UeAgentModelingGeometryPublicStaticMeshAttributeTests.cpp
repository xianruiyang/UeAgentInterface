#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "Composition/SimpleAuthoring/Families/Modeling/UeAgentModelingSimpleAuthoringProduction.h"
#include "Composition/SimpleAuthoring/OperationHost/UeAgentSimpleAuthoringOperationHost.h"
#include "Core/StateSemantic/UeAgentStateRevisionJson.h"
#include "DomainAdapters/ModelingGeometryEffect/UeAgentModelingGeometryEffectCarrierRevision.h"
#include "DomainAdapters/ModelingGeometryEffect/UeAgentModelingGeometryPublicStaticMeshTestSupport.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMesh/DynamicMeshOverlay.h"
#include "DynamicMeshToMeshDescription.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "HAL/FileManager.h"
#include "Materials/Material.h"
#include "MeshDescriptionToDynamicMesh.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/SecureHash.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "PackageTools.h"
#include "PhysicsEngine/BodySetup.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "StaticMeshAttributes.h"
#include "TestSupport/UeAgentPublicCommandTestHarness.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace UE::UAI::Tests::ModelingGeometryPublicStaticMeshAttributes
{
	using namespace UE::Geometry;
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::CommandRegistry;
	using namespace UE::UAI::DomainAdapters::ModelingGeometryEffect;
	using namespace UE::UAI::StateSemantic;
	using namespace UE::UAI::Tests::ModelingGeometryPublicStaticMesh;
	static_assert(
		UAI_MODELING_SIMPLE_AUTHORING_ARTIFACT_READBACK_V1 == 1,
		"Modeling artifact readback contract changed");

	constexpr int32 RequestedUvChannel = 2;
	constexpr int32 ProjectedUvChannel = 3;

	struct FUvChannelObservation final
	{
		int32 LayerCount = 0;
		int32 TriangleCount = 0;
		int32 SetTriangleCount = 0;
		int32 ElementCount = 0;
		FVector2f Min = FVector2f::ZeroVector;
		FVector2f Max = FVector2f::ZeroVector;
		bool bHasValue = false;
		bool bAllFinite = true;

		bool operator==(const FUvChannelObservation& Other) const
		{
			return LayerCount == Other.LayerCount
				&& TriangleCount == Other.TriangleCount
				&& SetTriangleCount == Other.SetTriangleCount
				&& ElementCount == Other.ElementCount
				&& Min.Equals(Other.Min)
				&& Max.Equals(Other.Max)
				&& bHasValue == Other.bHasValue
				&& bAllFinite == Other.bAllFinite;
		}
	};

	struct FMaterialSlotObservation final
	{
		TArray<FName> Names;
		TArray<FString> MaterialRefs;
		TMap<int32, int32> MaterialIdByTriangle;
	};

	struct FCollisionObservation final
	{
		int32 BoxCount = 0;
		int32 SphereCount = 0;
		int32 CapsuleCount = 0;
		int32 ConvexCount = 0;
		int32 ConvexVertexCount = 0;
		FBox Bounds = FBox(EForceInit::ForceInit);

		int32 ShapeCount() const
		{
			return BoxCount + SphereCount + CapsuleCount + ConvexCount;
		}
	};

	bool ReadUvChannel(
		UStaticMesh& StaticMesh,
		const int32 Channel,
		FUvChannelObservation& Out)
	{
		Out = {};
		const FMeshDescription* Description = StaticMesh.GetMeshDescription(0);
		if (!Description)
		{
			return false;
		}
		FDynamicMesh3 Mesh;
		FMeshDescriptionToDynamicMesh Converter;
		Converter.Convert(Description, Mesh);
		Out.TriangleCount = Mesh.TriangleCount();
		if (!Mesh.HasAttributes())
		{
			return true;
		}
		const FDynamicMeshAttributeSet* Attributes = Mesh.Attributes();
		Out.LayerCount = Attributes->NumUVLayers();
		if (Channel < 0 || Channel >= Out.LayerCount)
		{
			return true;
		}
		const FDynamicMeshUVOverlay* Uv = Attributes->GetUVLayer(Channel);
		if (!Uv)
		{
			return false;
		}
		Out.ElementCount = Uv->ElementCount();
		for (const int32 TriangleId : Mesh.TriangleIndicesItr())
		{
			Out.SetTriangleCount += Uv->IsSetTriangle(TriangleId) ? 1 : 0;
		}
		for (const int32 ElementId : Uv->ElementIndicesItr())
		{
			const FVector2f Value = Uv->GetElement(ElementId);
			Out.bAllFinite = Out.bAllFinite
				&& FMath::IsFinite(Value.X)
				&& FMath::IsFinite(Value.Y);
			if (!Out.bHasValue)
			{
				Out.Min = Value;
				Out.Max = Value;
				Out.bHasValue = true;
			}
			else
			{
				Out.Min.X = FMath::Min(Out.Min.X, Value.X);
				Out.Min.Y = FMath::Min(Out.Min.Y, Value.Y);
				Out.Max.X = FMath::Max(Out.Max.X, Value.X);
				Out.Max.Y = FMath::Max(Out.Max.Y, Value.Y);
			}
		}
		return Out.TriangleCount > 0;
	}

	bool ReadMaterialSlots(
		UStaticMesh& StaticMesh,
		FMaterialSlotObservation& Out)
	{
		Out = {};
		for (const FStaticMaterial& Material : StaticMesh.GetStaticMaterials())
		{
			Out.Names.Add(Material.MaterialSlotName);
			Out.MaterialRefs.Add(
				Material.MaterialInterface
					? Material.MaterialInterface->GetPathName()
					: FString());
		}
		const FMeshDescription* Description = StaticMesh.GetMeshDescription(0);
		if (!Description)
		{
			return false;
		}
		FDynamicMesh3 Mesh;
		FMeshDescriptionToDynamicMesh Converter;
		Converter.Convert(Description, Mesh);
		if (Mesh.HasAttributes()
			&& Mesh.Attributes()->HasMaterialID())
		{
			const FDynamicMeshMaterialAttribute* MaterialIds =
				Mesh.Attributes()->GetMaterialID();
			for (const int32 TriangleId : Mesh.TriangleIndicesItr())
			{
				Out.MaterialIdByTriangle.Add(
					TriangleId,
					MaterialIds->GetValue(TriangleId));
			}
		}
		return Out.Names.Num() == Out.MaterialRefs.Num();
	}

	bool ReadCollision(
		UStaticMesh& StaticMesh,
		FCollisionObservation& Out)
	{
		Out = {};
		const UBodySetup* const BodySetup = StaticMesh.GetBodySetup();
		if (!BodySetup)
		{
			return false;
		}
		const FKAggregateGeom& Geometry = BodySetup->AggGeom;
		Out.BoxCount = Geometry.BoxElems.Num();
		Out.SphereCount = Geometry.SphereElems.Num();
		Out.CapsuleCount = Geometry.SphylElems.Num()
			+ Geometry.TaperedCapsuleElems.Num();
		Out.ConvexCount = Geometry.ConvexElems.Num();
		for (const FKConvexElem& Convex : Geometry.ConvexElems)
		{
			Out.ConvexVertexCount += Convex.VertexData.Num();
		}
		if (Out.ShapeCount() > 0)
		{
			Out.Bounds = Geometry.CalcAABB(FTransform::Identity);
		}
		return true;
	}

	TSharedRef<FJsonObject> AutoUvParams(
		const FString& TargetRef)
	{
		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target_ref"), TargetRef);
		Params->SetNumberField(TEXT("uv_channel"), RequestedUvChannel);
		Params->SetStringField(TEXT("method"), TEXT("patch_builder"));
		Params->SetStringField(TEXT("island_source"), TEXT("polygroups"));
		Params->SetBoolField(TEXT("pack"), true);
		Params->SetNumberField(TEXT("texture_resolution"), 512.0);
		return Params;
	}

	TArray<TSharedPtr<FJsonValue>> Numbers(
		std::initializer_list<double> Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		Result.Reserve(static_cast<int32>(Values.size()));
		for (const double Value : Values)
		{
			Result.Add(MakeShared<FJsonValueNumber>(Value));
		}
		return Result;
	}

	TSharedRef<FJsonObject> ProjectUvParams(
		const FString& TargetRef,
		const TSharedRef<FJsonObject>& Revision)
	{
		const TSharedRef<FJsonObject> Frame = MakeShared<FJsonObject>();
		Frame->SetStringField(TEXT("frame_id"), TEXT("ue.mesh.target_local"));
		Frame->SetStringField(TEXT("unit"), TEXT("centimeter"));
		Frame->SetArrayField(TEXT("origin"), Numbers({0.0, 0.0, 0.0}));
		Frame->SetArrayField(TEXT("axis_u"), Numbers({1.0, 0.0, 0.0}));
		Frame->SetArrayField(TEXT("axis_v"), Numbers({0.0, 1.0, 0.0}));
		Frame->SetArrayField(TEXT("axis_w"), Numbers({0.0, 0.0, 1.0}));
		Frame->SetStringField(
			TEXT("basis_evidence"),
			TEXT("exact target-local orthonormal basis"));

		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target_ref"), TargetRef);
		Params->SetObjectField(
			TEXT("space_contract"),
			BuildTargetLocalSpaceContract());
		Params->SetNumberField(TEXT("uv_channel"), ProjectedUvChannel);
		Params->SetStringField(TEXT("projection_type"), TEXT("box"));
		Params->SetArrayField(
			TEXT("dimensions_cm"),
			Numbers({200.0, 100.0, 50.0}));
		Params->SetObjectField(TEXT("projection_frame"), Frame);
		return Params;
	}

	TSharedRef<FJsonObject> MaterialSlotAddParams(
		const FString& TargetRef,
		const TSharedRef<FJsonObject>& Revision,
		const FString& SlotName,
		const FString& MaterialRef)
	{
		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target_ref"), TargetRef);
		Params->SetStringField(TEXT("slot_name"), SlotName);
		Params->SetStringField(TEXT("material_ref"), MaterialRef);
		return Params;
	}

	TSharedRef<FJsonObject> MaterialSlotAssignParams(
		const FString& TargetRef,
		const TSharedRef<FJsonObject>& Revision,
		const int32 SlotIndex,
		const TArray<int32>& TriangleIds)
	{
		TArray<TSharedPtr<FJsonValue>> ElementIds;
		ElementIds.Reserve(TriangleIds.Num());
		for (const int32 TriangleId : TriangleIds)
		{
			ElementIds.Add(MakeShared<FJsonValueNumber>(TriangleId));
		}
		const TSharedRef<FJsonObject> Selection = MakeShared<FJsonObject>();
		Selection->SetStringField(TEXT("element_kind"), TEXT("triangle"));
		Selection->SetArrayField(TEXT("element_ids"), MoveTemp(ElementIds));

		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target_ref"), TargetRef);
		Params->SetObjectField(TEXT("selection"), Selection);
		Params->SetNumberField(TEXT("slot_index"), SlotIndex);
		return Params;
	}

	TSharedRef<FJsonObject> SimpleCollisionParams(
		const FString& TargetRef,
		const TSharedRef<FJsonObject>& Revision)
	{
		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target_ref"), TargetRef);
		Params->SetStringField(TEXT("method"), TEXT("aligned_boxes"));
		Params->SetNumberField(TEXT("max_shapes"), 4);
		Params->SetBoolField(TEXT("replace_existing"), true);
		return Params;
	}

	TSharedRef<FJsonObject> ConvexCollisionParams(
		const FString& TargetRef,
		const TSharedRef<FJsonObject>& Revision)
	{
		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target_ref"), TargetRef);
		Params->SetStringField(TEXT("mode"), TEXT("single_hull"));
		Params->SetNumberField(TEXT("max_hulls"), 4);
		Params->SetNumberField(TEXT("max_vertices_per_hull"), 64);
		Params->SetBoolField(TEXT("replace_existing"), true);
		return Params;
	}

	bool RunCurrentUvProductCase(
		FAutomationTestBase& Test,
		const TSharedPtr<const FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const FUeAgentExactCommandDispatch>& Dispatch,
		UWorld& World,
		const FString& CommandId,
		const int32 Channel,
		const FString& Scope,
		const FBuildParams& BuildParams)
	{
		FUvChannelObservation Before;
		FUvChannelObservation After;
		const auto VerifyBefore =
			[&Before, Channel](UStaticMesh& StaticMesh)
			{
				return ReadUvChannel(StaticMesh, Channel, Before)
					&& (Before.LayerCount <= Channel
						|| Before.ElementCount == 0
						|| Before.SetTriangleCount == 0);
			};
		const auto VerifyAfter =
			[&After, Channel](UStaticMesh& StaticMesh)
			{
				return ReadUvChannel(StaticMesh, Channel, After)
					&& After.TriangleCount > 0
					&& After.SetTriangleCount == After.TriangleCount
					&& After.ElementCount > 0
					&& After.bHasValue
					&& After.bAllFinite
					&& After.Min.X >= -0.001f
					&& After.Min.Y >= -0.001f
					&& After.Max.X <= 1.001f
					&& After.Max.Y <= 1.001f;
			};
		const auto PrepareCarrier =
			[](UStaticMesh&)
			{
				return true;
			};
		return RunCurrentStaticMeshProductCase(
			Test,
			Registry,
			Dispatch,
			World,
			CommandId,
			Scope,
			PrepareCarrier,
			BuildParams,
			VerifyBefore,
			VerifyAfter);
	}

	bool RunCurrentMaterialSlotAddProductCase(
		FAutomationTestBase& Test,
		const TSharedPtr<const FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const FUeAgentExactCommandDispatch>& Dispatch,
		UWorld& World)
	{
		const FString Scope(TEXT("T50PublicMaterialSlotAddCurrent"));
		const FString SlotName(TEXT("T50_Public_Added_Slot"));
		UMaterialInterface* const Material =
			UMaterial::GetDefaultMaterial(MD_Surface);
		if (!Test.TestNotNull(TEXT("the default surface material exists"), Material))
		{
			return false;
		}
		const FString MaterialRef = Material->GetPathName();
		FMaterialSlotObservation Before;
		FMaterialSlotObservation After;
		const auto BuildParams =
			[&SlotName, &MaterialRef](
				const FString& TargetRef,
				const TSharedRef<FJsonObject>& Revision)
			{
				return MaterialSlotAddParams(
					TargetRef,
					Revision,
					SlotName,
					MaterialRef);
			};
		const auto VerifyBefore =
			[&Before, &SlotName](UStaticMesh& StaticMesh)
			{
				return ReadMaterialSlots(StaticMesh, Before)
					&& !Before.Names.Contains(FName(*SlotName));
			};
		const auto VerifyAfter =
			[&Before, &After, &SlotName, &MaterialRef](
				UStaticMesh& StaticMesh)
			{
				return ReadMaterialSlots(StaticMesh, After)
					&& After.Names.Num() == Before.Names.Num() + 1
					&& After.Names.Last() == FName(*SlotName)
					&& After.MaterialRefs.Last() == MaterialRef;
			};
		const auto PrepareCarrier =
			[](UStaticMesh&)
			{
				return true;
			};
		return RunCurrentStaticMeshProductCase(
			Test,
			Registry,
			Dispatch,
			World,
			TEXT("mesh.attribute.material_slot.add"),
			Scope,
			PrepareCarrier,
			BuildParams,
			VerifyBefore,
			VerifyAfter);
	}

	bool RunCurrentMaterialSlotAssignProductCase(
		FAutomationTestBase& Test,
		const TSharedPtr<const FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const FUeAgentExactCommandDispatch>& Dispatch,
		UWorld& World)
	{
		const FString Scope(TEXT("T50PublicMaterialSlotAssignCurrent"));
		constexpr int32 TargetSlotIndex = 6;
		const TArray<int32> SelectedTriangles({0, 1, 4});
		UMaterialInterface* const Material =
			UMaterial::GetDefaultMaterial(MD_Surface);
		if (!Test.TestNotNull(TEXT("the default surface material exists"), Material))
		{
			return false;
		}
		FMaterialSlotObservation Before;
		FMaterialSlotObservation After;
		const auto PrepareCarrier =
			[Material](UStaticMesh& StaticMesh)
			{
				TArray<FStaticMaterial>& Materials =
					StaticMesh.GetStaticMaterials();
				Materials.Reset();
				for (int32 Index = 0; Index <= TargetSlotIndex; ++Index)
				{
					const FName SlotName(*FString::Printf(
						TEXT("T50_Assign_Slot_%d"),
						Index));
					Materials.Add(FStaticMaterial(
						Material,
						SlotName,
						SlotName));
				}
				return Materials.Num() == TargetSlotIndex + 1;
			};
		const auto BuildParams =
			[&SelectedTriangles](
				const FString& TargetRef,
				const TSharedRef<FJsonObject>& Revision)
			{
				return MaterialSlotAssignParams(
					TargetRef,
					Revision,
					TargetSlotIndex,
					SelectedTriangles);
			};
		const auto VerifyBefore =
			[&Before, &SelectedTriangles](UStaticMesh& StaticMesh)
			{
				if (!ReadMaterialSlots(StaticMesh, Before)
					|| Before.Names.Num() != TargetSlotIndex + 1)
				{
					return false;
				}
				for (const int32 TriangleId : SelectedTriangles)
				{
					const int32* MaterialId =
						Before.MaterialIdByTriangle.Find(TriangleId);
					if (!MaterialId || *MaterialId == TargetSlotIndex)
					{
						return false;
					}
				}
				return true;
			};
		const auto VerifyAfter =
			[&Before, &After, &SelectedTriangles](UStaticMesh& StaticMesh)
			{
				if (!ReadMaterialSlots(StaticMesh, After)
					|| After.Names != Before.Names
					|| After.MaterialRefs != Before.MaterialRefs)
				{
					return false;
				}
				for (const TPair<int32, int32>& Pair
					: Before.MaterialIdByTriangle)
				{
					const int32* MaterialId =
						After.MaterialIdByTriangle.Find(Pair.Key);
					const int32 Expected = SelectedTriangles.Contains(Pair.Key)
						? TargetSlotIndex
						: Pair.Value;
					if (!MaterialId || *MaterialId != Expected)
					{
						return false;
					}
				}
				return After.MaterialIdByTriangle.Num()
					== Before.MaterialIdByTriangle.Num();
			};
		return RunCurrentStaticMeshProductCase(
			Test,
			Registry,
			Dispatch,
			World,
			TEXT("mesh.attribute.material_slot.assign"),
			Scope,
			PrepareCarrier,
			BuildParams,
			VerifyBefore,
			VerifyAfter);
	}

	bool RunCurrentSimpleCollisionProductCase(
		FAutomationTestBase& Test,
		const TSharedPtr<const FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const FUeAgentExactCommandDispatch>& Dispatch,
		UWorld& World)
	{
		const FString Scope(TEXT("T50PublicSimpleCollisionCurrent"));
		FCollisionObservation Before;
		FCollisionObservation After;
		const auto PrepareCarrier =
			[](UStaticMesh& StaticMesh)
			{
				UBodySetup* const BodySetup = StaticMesh.GetBodySetup();
				if (!BodySetup)
				{
					return false;
				}
				BodySetup->AggGeom.EmptyElements();
				FKSphereElem Sphere;
				Sphere.Center = FVector(20.0, 0.0, 0.0);
				Sphere.Radius = 12.5;
				BodySetup->AggGeom.SphereElems.Add(Sphere);
				BodySetup->InvalidatePhysicsData();
				BodySetup->CreatePhysicsMeshes();
				return true;
			};
		const auto BuildParams =
			[](const FString& TargetRef,
				const TSharedRef<FJsonObject>& Revision)
			{
				return SimpleCollisionParams(TargetRef, Revision);
			};
		const auto VerifyBefore =
			[&Before](UStaticMesh& StaticMesh)
			{
				return ReadCollision(StaticMesh, Before)
					&& Before.ShapeCount() == 1
					&& Before.SphereCount == 1
					&& Before.BoxCount == 0
					&& Before.ConvexCount == 0;
			};
		const auto VerifyAfter =
			[&After](UStaticMesh& StaticMesh)
			{
				return ReadCollision(StaticMesh, After)
					&& After.ShapeCount() == 1
					&& After.BoxCount == 1
					&& After.SphereCount == 0
					&& After.CapsuleCount == 0
					&& After.ConvexCount == 0
					&& After.Bounds.IsValid;
			};
		return RunCurrentStaticMeshProductCase(
			Test,
			Registry,
			Dispatch,
			World,
			TEXT("mesh.collision.simple.generate"),
			Scope,
			PrepareCarrier,
			BuildParams,
			VerifyBefore,
			VerifyAfter);
	}

	bool RunCurrentConvexCollisionProductCase(
		FAutomationTestBase& Test,
		const TSharedPtr<const FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const FUeAgentExactCommandDispatch>& Dispatch,
		UWorld& World)
	{
		const FString Scope(TEXT("T50PublicConvexCollisionCurrent"));
		FCollisionObservation Before;
		FCollisionObservation After;
		const auto PrepareCarrier =
			[](UStaticMesh& StaticMesh)
			{
				UBodySetup* const BodySetup = StaticMesh.GetBodySetup();
				if (!BodySetup)
				{
					return false;
				}
				BodySetup->AggGeom.EmptyElements();
				FKSphereElem Sphere;
				Sphere.Center = FVector(-20.0, 0.0, 0.0);
				Sphere.Radius = 10.0;
				BodySetup->AggGeom.SphereElems.Add(Sphere);
				BodySetup->InvalidatePhysicsData();
				BodySetup->CreatePhysicsMeshes();
				return true;
			};
		const auto BuildParams =
			[](const FString& TargetRef,
				const TSharedRef<FJsonObject>& Revision)
			{
				return ConvexCollisionParams(TargetRef, Revision);
			};
		const auto VerifyBefore =
			[&Before](UStaticMesh& StaticMesh)
			{
				return ReadCollision(StaticMesh, Before)
					&& Before.ShapeCount() == 1
					&& Before.SphereCount == 1
					&& Before.ConvexCount == 0;
			};
		const auto VerifyAfter =
			[&After](UStaticMesh& StaticMesh)
			{
				return ReadCollision(StaticMesh, After)
					&& After.ShapeCount() == 1
					&& After.ConvexCount == 1
					&& After.BoxCount == 0
					&& After.SphereCount == 0
					&& After.CapsuleCount == 0
					&& After.ConvexVertexCount >= 4
					&& After.ConvexVertexCount <= 64
					&& After.Bounds.IsValid;
			};
		return RunCurrentStaticMeshProductCase(
			Test,
			Registry,
			Dispatch,
			World,
			TEXT("mesh.collision.convex.generate"),
			Scope,
			PrepareCarrier,
			BuildParams,
			VerifyBefore,
			VerifyAfter);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentModelingGeometryPublicStaticMeshAutoUvTest,
	"UeAgentInterface.DomainAdapters.ModelingGeometryEffect.E4.PublicProduct."
		"StaticMeshAutoUv",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentModelingGeometryPublicStaticMeshAutoUvTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	using namespace UE::UAI::Tests::ModelingGeometryPublicStaticMeshAttributes;
	(void)Parameters;

	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	FString CompositionError;
	if (!TestTrue(
			TEXT("the active public composition freezes"),
			TryComposeProcessActiveCommandComposition(
				Registry,
				Dispatch,
				CompositionError))
		|| !Registry.IsValid()
		|| !Dispatch.IsValid())
	{
		AddError(CompositionError);
		return false;
	}
	UWorld* const World = GEditor
		? GEditor->GetEditorWorldContext().World()
		: nullptr;
	if (!TestNotNull(TEXT("the owned worker has an editor world"), World))
	{
		return false;
	}
	Composition::SimpleAuthoring::OperationHost::
		SetAutomationWorldOverride(World, true);
	ON_SCOPE_EXIT
	{
		Composition::SimpleAuthoring::OperationHost::
			SetAutomationWorldOverride(nullptr, true);
	};

	RunCurrentUvProductCase(
		*this,
		Registry,
		Dispatch,
		*World,
		TEXT("mesh.attribute.uv.auto"),
		RequestedUvChannel,
		TEXT("T50PublicAutoUvCurrent"),
		[](const FString& TargetRef,
			const TSharedRef<FJsonObject>& Revision)
		{
			return AutoUvParams(TargetRef);
		});
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentModelingGeometryPublicStaticMeshProjectUvTest,
	"UeAgentInterface.DomainAdapters.ModelingGeometryEffect.E4.PublicProduct."
		"StaticMeshProjectUv",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentModelingGeometryPublicStaticMeshProjectUvTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	using namespace UE::UAI::Tests::ModelingGeometryPublicStaticMeshAttributes;
	(void)Parameters;

	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	FString CompositionError;
	if (!TestTrue(
			TEXT("the active public composition freezes"),
			TryComposeProcessActiveCommandComposition(
				Registry,
				Dispatch,
				CompositionError))
		|| !Registry.IsValid()
		|| !Dispatch.IsValid())
	{
		AddError(CompositionError);
		return false;
	}
	UWorld* const World = GEditor
		? GEditor->GetEditorWorldContext().World()
		: nullptr;
	if (!TestNotNull(TEXT("the owned worker has an editor world"), World))
	{
		return false;
	}
	Composition::SimpleAuthoring::OperationHost::
		SetAutomationWorldOverride(World, true);
	ON_SCOPE_EXIT
	{
		Composition::SimpleAuthoring::OperationHost::
			SetAutomationWorldOverride(nullptr, true);
	};

	RunCurrentUvProductCase(
		*this,
		Registry,
		Dispatch,
		*World,
		TEXT("mesh.attribute.uv.project"),
		ProjectedUvChannel,
		TEXT("T50PublicProjectUvCurrent"),
		[](const FString& TargetRef,
			const TSharedRef<FJsonObject>& Revision)
		{
			return ProjectUvParams(TargetRef, Revision);
		});
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentModelingGeometryPublicStaticMeshMaterialSlotAddTest,
	"UeAgentInterface.DomainAdapters.ModelingGeometryEffect.E4.PublicProduct."
		"StaticMeshMaterialSlotAdd",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentModelingGeometryPublicStaticMeshMaterialSlotAddTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	using namespace UE::UAI::Tests::ModelingGeometryPublicStaticMeshAttributes;
	(void)Parameters;

	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	FString CompositionError;
	if (!TestTrue(
			TEXT("the active public composition freezes"),
			TryComposeProcessActiveCommandComposition(
				Registry,
				Dispatch,
				CompositionError))
		|| !Registry.IsValid()
		|| !Dispatch.IsValid())
	{
		AddError(CompositionError);
		return false;
	}
	UWorld* const World = GEditor
		? GEditor->GetEditorWorldContext().World()
		: nullptr;
	if (!TestNotNull(TEXT("the owned worker has an editor world"), World))
	{
		return false;
	}
	Composition::SimpleAuthoring::OperationHost::
		SetAutomationWorldOverride(World, true);
	ON_SCOPE_EXIT
	{
		Composition::SimpleAuthoring::OperationHost::
			SetAutomationWorldOverride(nullptr, true);
	};

	RunCurrentMaterialSlotAddProductCase(
		*this,
		Registry,
		Dispatch,
		*World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentModelingGeometryPublicStaticMeshMaterialSlotAssignTest,
	"UeAgentInterface.DomainAdapters.ModelingGeometryEffect.E4.PublicProduct."
		"StaticMeshMaterialSlotAssign",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentModelingGeometryPublicStaticMeshMaterialSlotAssignTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	using namespace UE::UAI::Tests::ModelingGeometryPublicStaticMeshAttributes;
	(void)Parameters;

	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	FString CompositionError;
	if (!TestTrue(
			TEXT("the active public composition freezes"),
			TryComposeProcessActiveCommandComposition(
				Registry,
				Dispatch,
				CompositionError))
		|| !Registry.IsValid()
		|| !Dispatch.IsValid())
	{
		AddError(CompositionError);
		return false;
	}
	UWorld* const World = GEditor
		? GEditor->GetEditorWorldContext().World()
		: nullptr;
	if (!TestNotNull(TEXT("the owned worker has an editor world"), World))
	{
		return false;
	}
	Composition::SimpleAuthoring::OperationHost::
		SetAutomationWorldOverride(World, true);
	ON_SCOPE_EXIT
	{
		Composition::SimpleAuthoring::OperationHost::
			SetAutomationWorldOverride(nullptr, true);
	};

	RunCurrentMaterialSlotAssignProductCase(
		*this,
		Registry,
		Dispatch,
		*World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentModelingGeometryPublicStaticMeshSimpleCollisionTest,
	"UeAgentInterface.DomainAdapters.ModelingGeometryEffect.E4.PublicProduct."
		"StaticMeshSimpleCollision",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentModelingGeometryPublicStaticMeshSimpleCollisionTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	using namespace UE::UAI::Tests::ModelingGeometryPublicStaticMeshAttributes;
	(void)Parameters;

	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	FString CompositionError;
	if (!TestTrue(
			TEXT("the active public composition freezes"),
			TryComposeProcessActiveCommandComposition(
				Registry,
				Dispatch,
				CompositionError))
		|| !Registry.IsValid()
		|| !Dispatch.IsValid())
	{
		AddError(CompositionError);
		return false;
	}
	UWorld* const World = GEditor
		? GEditor->GetEditorWorldContext().World()
		: nullptr;
	if (!TestNotNull(TEXT("the owned worker has an editor world"), World))
	{
		return false;
	}
	Composition::SimpleAuthoring::OperationHost::
		SetAutomationWorldOverride(World, true);
	ON_SCOPE_EXIT
	{
		Composition::SimpleAuthoring::OperationHost::
			SetAutomationWorldOverride(nullptr, true);
	};

	RunCurrentSimpleCollisionProductCase(
		*this,
		Registry,
		Dispatch,
		*World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentModelingGeometryPublicStaticMeshConvexCollisionTest,
	"UeAgentInterface.DomainAdapters.ModelingGeometryEffect.E4.PublicProduct."
		"StaticMeshConvexCollision",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentModelingGeometryPublicStaticMeshConvexCollisionTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	using namespace UE::UAI::Tests::ModelingGeometryPublicStaticMeshAttributes;
	(void)Parameters;

	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	FString CompositionError;
	if (!TestTrue(
			TEXT("the active public composition freezes"),
			TryComposeProcessActiveCommandComposition(
				Registry,
				Dispatch,
				CompositionError))
		|| !Registry.IsValid()
		|| !Dispatch.IsValid())
	{
		AddError(CompositionError);
		return false;
	}
	UWorld* const World = GEditor
		? GEditor->GetEditorWorldContext().World()
		: nullptr;
	if (!TestNotNull(TEXT("the owned worker has an editor world"), World))
	{
		return false;
	}
	Composition::SimpleAuthoring::OperationHost::
		SetAutomationWorldOverride(World, true);
	ON_SCOPE_EXIT
	{
		Composition::SimpleAuthoring::OperationHost::
			SetAutomationWorldOverride(nullptr, true);
	};

	RunCurrentConvexCollisionProductCase(
		*this,
		Registry,
		Dispatch,
		*World);
	return !HasAnyErrors();
}

#endif

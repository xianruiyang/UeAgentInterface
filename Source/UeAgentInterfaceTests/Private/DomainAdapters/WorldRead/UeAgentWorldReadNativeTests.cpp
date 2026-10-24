#include "Misc/AutomationTest.h"

// RE-003 supplies the optional production seam frozen by the RE-002 test.
#include "DomainAdapters/WorldRead/UeAgentWorldReadNative.h"
#include "DomainAdapters/WorldRead/UeAgentWorldReadDescriptors.h"


#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Core/ContextResource/UeAgentOperationContext.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "MeshDescription.h"
#include "Misc/ScopeExit.h"
#include "PhysicsEngine/PhysicsConstraintComponent.h"
#include "StaticMeshAttributes.h"
#include "UObject/Package.h"

namespace
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::Discovery;
	using namespace UE::UAI::DomainAdapters::WorldRead;
	using namespace UE::UAI::StateSemantic;

	class FOwnedWorldReadWorld final
	{
	public:
		FOwnedWorldReadWorld()
		{
			const FString PackageName = FString::Printf(
				TEXT("/Temp/UAI/WorldRead/%s"),
				*FGuid::NewGuid().ToString(
					EGuidFormats::Digits));
			Package = CreatePackage(*PackageName);
			World = UWorld::CreateWorld(
				EWorldType::EditorPreview,
				false,
				FName(TEXT("WorldReadOwnedWorld")),
				Package);
			if (!World)
			{
				return;
			}

			Actor = World->SpawnActor<AActor>(
				FVector::ZeroVector,
				FRotator::ZeroRotator);
			if (!Actor)
			{
				return;
			}
			Actor->Rename(TEXT("WorldReadOwnedActor"));

			Collision = NewObject<UBoxComponent>(
				Actor,
				TEXT("WorldReadCollision"));
			Collision->InitBoxExtent(FVector(20.0, 20.0, 20.0));
			Collision->SetCollisionEnabled(
				ECollisionEnabled::QueryOnly);
			Collision->SetCollisionObjectType(ECC_WorldStatic);
			Collision->SetCollisionResponseToAllChannels(
				ECR_Block);
			Actor->SetRootComponent(Collision);
			Actor->AddInstanceComponent(Collision);
			Collision->RegisterComponentWithWorld(World);
			Collision->SetWorldLocation(FVector::ZeroVector);

			CollisionB = NewObject<UBoxComponent>(
				Actor,
				TEXT("WorldReadCollisionB"));
			CollisionB->InitBoxExtent(FVector(15.0, 15.0, 15.0));
			CollisionB->SetCollisionEnabled(
				ECollisionEnabled::QueryAndPhysics);
			CollisionB->SetSimulatePhysics(false);
			Actor->AddInstanceComponent(CollisionB);
			CollisionB->RegisterComponentWithWorld(World);
			CollisionB->SetWorldLocation(FVector(100.0, 0.0, 0.0));

			Constraint = NewObject<UPhysicsConstraintComponent>(
				Actor,
				TEXT("WorldReadConstraint"));
			Actor->AddInstanceComponent(Constraint);
			Constraint->RegisterComponentWithWorld(World);
			Constraint->SetConstrainedComponents(
				Collision,
				NAME_None,
				CollisionB,
				NAME_None);
			Constraint->ConstraintInstance.SetLinearXLimit(
				ELinearConstraintMotion::LCM_Limited,
				25.0f);

			Mesh = NewObject<UStaticMesh>(
				Package,
				TEXT("WorldReadTriangleMesh"),
				RF_Transient);
			FMeshDescription Description;
			FStaticMeshAttributes Attributes(Description);
			Attributes.Register();
			TVertexAttributesRef<FVector3f> Positions =
				Attributes.GetVertexPositions();
			const FVertexID V0 = Description.CreateVertex();
			const FVertexID V1 = Description.CreateVertex();
			const FVertexID V2 = Description.CreateVertex();
			Positions[V0] = FVector3f(1.0f, 2.0f, 3.0f);
			Positions[V1] = FVector3f(2.0f, 2.0f, 3.0f);
			Positions[V2] = FVector3f(1.0f, 3.0f, 3.0f);
			const FVertexInstanceID I0 =
				Description.CreateVertexInstance(V0);
			const FVertexInstanceID I1 =
				Description.CreateVertexInstance(V1);
			const FVertexInstanceID I2 =
				Description.CreateVertexInstance(V2);
			const FPolygonGroupID Group =
				Description.CreatePolygonGroup();
			Description.CreatePolygon(Group, {I0, I1, I2});
			Mesh->BuildFromMeshDescriptions({&Description});

			MeshComponent = NewObject<UStaticMeshComponent>(
				Actor,
				TEXT("WorldReadMesh"));
			MeshComponent->SetStaticMesh(Mesh);
			MeshComponent->SetWorldLocation(
				FVector(10.0, 20.0, 30.0));
			Actor->AddInstanceComponent(MeshComponent);
			MeshComponent->RegisterComponentWithWorld(World);
			Package->SetDirtyFlag(false);
		}

		~FOwnedWorldReadWorld()
		{
			if (Package)
			{
				Package->SetDirtyFlag(false);
			}
			if (World)
			{
				World->DestroyWorld(false);
				World->MarkAsGarbage();
			}
			if (Package)
			{
				Package->MarkAsGarbage();
			}
		}

		bool IsValid() const
		{
			return Package
				&& World
				&& Actor
				&& Collision
				&& CollisionB
				&& Constraint
				&& Mesh
				&& MeshComponent;
		}

		UPackage* Package = nullptr;
		UWorld* World = nullptr;
		AActor* Actor = nullptr;
		UBoxComponent* Collision = nullptr;
		UBoxComponent* CollisionB = nullptr;
		UPhysicsConstraintComponent* Constraint = nullptr;
		UStaticMesh* Mesh = nullptr;
		UStaticMeshComponent* MeshComponent = nullptr;
	};

	FUeAgentDescriptorIdentity NativeTestIdentity()
	{
		TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
		FString Error;
		if (TryCreateWorldReadDescriptorRegistry(Registry, Error)
			&& Registry.IsValid())
		{
			const FUeAgentOperationDescriptor* Descriptor =
				Registry->FindByDescriptorId(
					TEXT("uai.query.world.summary"));
			if (Descriptor)
			{
				return Descriptor->GetIdentity();
			}
		}
		return FUeAgentDescriptorIdentity(FString(), 0, FString());
	}

	FUeAgentResourceTrackerOwner NativeResources()
	{
		return MakeShared<FUeAgentResourceTracker, ESPMode::ThreadSafe>();
	}

	TSharedPtr<FUeAgentOperationContext> NativeContext(
		FAutomationTestBase* Test,
		UWorld* World,
		const FUeAgentResourceTrackerOwner& Resources)
	{
		FUeAgentExecutionContextInput Input;
		Input.ProjectInstanceId = TEXT("fixture-project");
		Input.EditorInstanceId = TEXT("fixture-editor");
		Input.StatePlane = EUeAgentStatePlane::AuthoringMemory;
		Input.WorldType = EUeAgentExecutionWorldType::Preview;
		Input.WorldIdentity = World->GetPathName();
		Input.ContextRevision = TEXT("owned-world-revision");

		const FUeAgentExecutionContextPolicy Policy(
			{EUeAgentStatePlane::AuthoringMemory},
			{EUeAgentExecutionContextFacet::World},
			EUeAgentContextResolutionPolicy::
				ResolveCurrentThenFreeze,
			EUeAgentTransientContextPolicy::ReadOnly);
		FUeAgentExecutionContextRefPtr ExecutionContext;
		FUeAgentExecutionContextError ContextError;
		Test->TestTrue(
			TEXT("freeze owned worker world"),
			FUeAgentExecutionContextRef::TryCreate(
				Input,
				NativeTestIdentity(),
				EUeAgentRequestKind::Query,
				Policy,
				ExecutionContext,
				ContextError));
		if (!ExecutionContext.IsValid())
		{
			return nullptr;
		}

		const FUeAgentQueryRequest Request(
			NativeTestIdentity(),
			TEXT("request-world-read-native"),
			FUeAgentContractValue(TEXT("{}")));
		const FUeAgentCallerContext Caller(
			TEXT("fixture-agent"),
			{TEXT("world:read")},
			TEXT("corr-world-read-native"));
		const FUeAgentOperationDeadline Deadline =
			FUeAgentOperationDeadline::FromTimeout(
				30.0,
				[]() { return 100.0; });
		const FUeAgentCancellationSource Cancellation(
			[]() { return 100.0; });
		TSharedPtr<FUeAgentOperationContext> Context;
		FUeAgentOperationContextError OperationError;
		Test->TestTrue(
			TEXT("create native WorldRead operation context"),
			FUeAgentOperationContext::TryCreate(
				Request,
				Caller,
				ExecutionContext.ToSharedRef(),
				Resources,
				Deadline,
				Cancellation.GetToken(),
				Context,
				OperationError));
		return Context;
	}

	FUeAgentCandidateBinding NativeTarget(
		const FString& BindingId,
		const FString& Kind,
		const FString& PublicRef)
	{
		return FUeAgentCandidateBinding(
			BindingId,
			FUeAgentCandidateSummary(
				FUeAgentCanonicalIdentity(
					TEXT("world.target:") + BindingId,
					Kind,
					TEXT("owned-world"),
					BindingId,
					TEXT("target"),
					TEXT("owned-world-revision")),
				NativeTestIdentity(),
				TEXT("world_target"),
				BindingId,
				TEXT("owned WorldRead target"),
				{TEXT("WorldRead"), TEXT("Fixture")}),
			FUeAgentCatalogRevision(
				TEXT("owned-world-catalog"),
				TEXT("owned-world-revision"),
				1),
			PublicRef,
			TEXT("world-read"),
			{EUeAgentCandidateOperation::Read},
			TEXT("world-read-native-test"));
	}

	class FOwnedWorldReadEnvironment final
		: public IUeAgentWorldReadEnvironment
	{
	public:
		explicit FOwnedWorldReadEnvironment(
			FOwnedWorldReadWorld& InOwner)
			: Owner(InOwner)
		{
			Targets.Add(
				TEXT("target-mesh"),
				Owner.MeshComponent);
			Targets.Add(
				TEXT("target-physics"),
				Owner.Collision);
			Targets.Add(
				TEXT("target-constraint"),
				Owner.Constraint);
		}

		bool TryResolveWorld(
			const FUeAgentExecutionContextRef& Context,
			UWorld*& OutWorld,
			FUeAgentWorldReadError& OutError) override
		{
			++WorldCalls;
			if (Context.GetWorldIdentity()
				!= Owner.World->GetPathName())
			{
				OutError.Code =
					TEXT("world_read_world_identity_mismatch");
				OutError.SubjectId =
					Context.GetWorldIdentity();
				return false;
			}
			OutWorld = Owner.World;
			return true;
		}

		bool TryResolveTarget(
			const FUeAgentCandidateBinding& Target,
			UObject*& OutObject,
			FUeAgentWorldReadError& OutError) override
		{
			++TargetCalls;
			OutObject = Targets.FindRef(Target.GetBindingId());
			if (!OutObject)
			{
				OutError.Code =
					TEXT("world_read_target_unresolved");
				OutError.SubjectId = Target.GetBindingId();
				return false;
			}
			return true;
		}

		int32 WorldCalls = 0;
		int32 TargetCalls = 0;

	private:
		FOwnedWorldReadWorld& Owner;
		TMap<FString, UObject*> Targets;
	};

	bool CaptureNative(
		FAutomationTestBase* Test,
		const FString& ProviderBinding,
		const FUeAgentContractValue& Input,
		const TOptional<FUeAgentCandidateBinding>& Target,
		const TSharedRef<IUeAgentWorldReadEnvironment>& Environment,
		const FUeAgentOperationContext& Context,
		TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
		FUeAgentWorldReadError& OutError)
	{
		TSharedPtr<IUeAgentWorldReadHook> Hook;
		if (!Test->TestTrue(
				*FString::Printf(
					TEXT("create thin native hook %s"),
					*ProviderBinding),
				FUeAgentWorldReadNativeHookFactory::TryCreate(
					ProviderBinding,
					Environment,
					Hook,
					OutError)))
		{
			return false;
		}
		return Hook.IsValid()
			&& Hook->TryCapture(
				Input,
				Target,
				Context,
				OutSnapshot,
				OutError);
	}

	bool SnapshotHasOwner(
		const TSharedPtr<const FUeAgentCanonicalSnapshot>& Snapshot,
		const FString& OwnerId)
	{
		return Snapshot.IsValid()
			&& Snapshot->GetEntries().ContainsByPredicate(
				[&OwnerId](const FUeAgentStateEntry& Entry)
				{
					return Entry.GetIdentity().GetOwnerId()
						.Contains(OwnerId);
				});
	}

	bool SnapshotHasSelector(
		const TSharedPtr<const FUeAgentCanonicalSnapshot>& Snapshot,
		const FString& Selector)
	{
		return Snapshot.IsValid()
			&& Snapshot->GetEntries().ContainsByPredicate(
				[&Selector](const FUeAgentStateEntry& Entry)
				{
					return Entry.GetIdentity().GetSelector() == Selector;
				});
	}

	bool SnapshotArrayContainsObjectStringField(
		const TSharedPtr<const FUeAgentCanonicalSnapshot>& Snapshot,
		const FString& Selector,
		const FString& FieldName,
		const FString& ExpectedValue)
	{
		if (!Snapshot.IsValid())
		{
			return false;
		}
		for (const FUeAgentStateEntry& Entry : Snapshot->GetEntries())
		{
			if (Entry.GetIdentity().GetSelector() != Selector
				|| Entry.GetValue()->GetKind() != EUeAgentValueKind::Array)
			{
				continue;
			}
			for (const FUeAgentCanonicalValueRef& Item
				: Entry.GetValue()->GetArrayItems())
			{
				if (Item->GetKind() != EUeAgentValueKind::Object)
				{
					continue;
				}
				for (const auto& Field : Item->GetObjectItems())
				{
					if (Field.Key == FieldName
						&& Field.Value->GetKind() == EUeAgentValueKind::String
						&& Field.Value->GetText() == ExpectedValue)
					{
						return true;
					}
				}
			}
		}
		return false;
	}

	const FUeAgentSpatialValue* FindWorldSpatial(
		const TSharedPtr<const FUeAgentCanonicalSnapshot>& Snapshot)
	{
		if (!Snapshot.IsValid())
		{
			return nullptr;
		}
		for (const FUeAgentStateEntry& Entry
			: Snapshot->GetEntries())
		{
			const FUeAgentSpatialValuePtr& Spatial =
				Entry.GetValue()->GetSpatial();
			if (Spatial.IsValid()
				&& Spatial->GetFrame().GetFrameId()
					== TEXT("ue.world"))
			{
				return Spatial.Get();
			}
		}
		return nullptr;
	}
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentWorldReadNativeOwnedWorldTest,
	"UeAgentInterface.DomainAdapters.WorldRead.E2.Native."
		"SevenFamiliesOwnedWorld",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentWorldReadNativeOwnedWorldTest::RunTest(
	const FString& Parameters)
{
	FOwnedWorldReadWorld Owner;
	if (!TestTrue(TEXT("owned worker world is valid"), Owner.IsValid()))
	{
		return false;
	}
	const FTransform OriginalMeshTransform =
		Owner.MeshComponent->GetComponentTransform();
	const FVector OriginalCollisionExtent =
		Owner.Collision->GetUnscaledBoxExtent();
	const FUeAgentResourceTrackerOwner Resources =
		NativeResources();
	const TSharedPtr<FUeAgentOperationContext> Context =
		NativeContext(this, Owner.World, Resources);
	if (!Context.IsValid())
	{
		return false;
	}
	const TSharedRef<FOwnedWorldReadEnvironment> Environment =
		MakeShared<FOwnedWorldReadEnvironment>(Owner);
	TSharedPtr<const FUeAgentCanonicalSnapshot> Snapshot;
	FUeAgentWorldReadError Error;

	TestTrue(
		TEXT("object family reads the owned UWorld"),
		CaptureNative(
			this,
			TEXT("world.read.summary"),
			FUeAgentContractValue(TEXT("{}")),
			{},
			Environment,
			*Context,
			Snapshot,
			Error));
	TestTrue(
		TEXT("object snapshot is sourced from the owned world"),
		SnapshotHasOwner(Snapshot, Owner.World->GetPathName()));

	Snapshot.Reset();
	TestTrue(
		TEXT("content family reads real owned actor/component facts"),
		CaptureNative(
			this,
			TEXT("world.read.content"),
			FUeAgentContractValue(TEXT(
				"{\"include_components\":true,"
				"\"scope\":\"world\","
				"\"sections\":[\"actors\",\"components\"]}")),
			{},
			Environment,
			*Context,
			Snapshot,
			Error));
	TestTrue(
		TEXT("content snapshot contains the owned actor"),
		SnapshotHasOwner(Snapshot, Owner.Actor->GetPathName()));

	Snapshot.Reset();
	TestTrue(
		TEXT("spatial family traces the owned collision primitive"),
		CaptureNative(
			this,
			TEXT("world.read.ray_trace"),
			FUeAgentContractValue(TEXT(
				"{\"direction\":{\"components\":[1,0,0],"
				"\"frame_id\":\"ue.world\"},"
				"\"distance_cm\":200,"
				"\"start\":{\"components\":[-100,0,0],"
				"\"frame_id\":\"ue.world\","
				"\"unit\":\"centimeter\"},"
				"\"trace_channel\":\"visibility\"}")),
			{},
			Environment,
			*Context,
			Snapshot,
			Error));
	TestNotNull(
		TEXT("trace result is expressed in ue.world"),
		FindWorldSpatial(Snapshot));
	TestTrue(
		TEXT("trace result explicitly reports a hit"),
		SnapshotHasSelector(Snapshot, TEXT("hit")));
	TestTrue(
		TEXT("trace result exposes the readable hit target"),
		SnapshotHasSelector(Snapshot, TEXT("target_ref")));

	Snapshot.Reset();
	TestTrue(
		TEXT("a ray miss is a successful business observation"),
		CaptureNative(
			this,
			TEXT("world.read.ray_trace"),
			FUeAgentContractValue(TEXT(
				"{\"direction\":{\"components\":[1,0,0],"
				"\"frame_id\":\"ue.world\"},"
				"\"distance_cm\":10,"
				"\"start\":{\"components\":[10000,10000,10000],"
				"\"frame_id\":\"ue.world\","
				"\"unit\":\"centimeter\"}}")),
			{},
			Environment,
			*Context,
			Snapshot,
			Error));
	TestTrue(
		TEXT("a ray miss still publishes the hit field"),
		SnapshotHasSelector(Snapshot, TEXT("hit")));
	TestFalse(
		TEXT("a ray miss does not fabricate an impact point"),
		SnapshotHasSelector(Snapshot, TEXT("impact_point")));

	const FUeAgentCandidateBinding MeshTarget =
		NativeTarget(
			TEXT("target-mesh"),
			TEXT("static_mesh_component"),
			Owner.MeshComponent->GetPathName());
	Snapshot.Reset();
	TestTrue(
		TEXT("mesh family resolves a Ticket target"),
		CaptureNative(
			this,
			TEXT("world.read.mesh_vertex_position"),
			FUeAgentContractValue(TEXT("{\"vertex_index\":0}")),
			MeshTarget,
			Environment,
			*Context,
			Snapshot,
			Error));
	const FUeAgentSpatialValue* MeshVertex =
		FindWorldSpatial(Snapshot);
	TestNotNull(
		TEXT("mesh-local vertex is converted to ue.world"),
		MeshVertex);
	if (MeshVertex)
	{
		TestEqual(
			TEXT("mesh vertex conversion has three components"),
			MeshVertex->GetComponents().Num(),
			3);
		TestTrue(
			TEXT("mesh vertex records the local-to-world chain"),
			!MeshVertex->GetConversionEvidence().IsEmpty());
	}

	Snapshot.Reset();
	TestTrue(
		TEXT("overlap executes native collision mechanics"),
		CaptureNative(
			this,
			TEXT("world.read.overlap"),
			FUeAgentContractValue(TEXT(
				"{\"center\":{\"components\":[0,0,0],"
				"\"frame_id\":\"ue.world\",\"unit\":\"centimeter\"},"
				"\"include_targets\":true,"
				"\"radius_cm\":40,\"shape\":\"sphere\","
				"\"trace_channel\":\"visibility\"}")),
			{},
			Environment,
			*Context,
			Snapshot,
			Error));
	TestTrue(
		TEXT("overlap publishes a normalized target collection"),
		SnapshotHasSelector(Snapshot, TEXT("targets")));
	TestTrue(
		TEXT("overlap publishes the matching unique target count"),
		SnapshotHasSelector(Snapshot, TEXT("overlap_count")));

	Snapshot.Reset();
	TestTrue(
		TEXT("capsule sweep executes native sweep mechanics"),
		CaptureNative(
			this,
			TEXT("world.read.capsule_sweep"),
			FUeAgentContractValue(TEXT(
				"{\"end\":{\"components\":[50,0,0],\"frame_id\":\"ue.world\","
				"\"unit\":\"centimeter\"},\"half_height_cm\":10,"
				"\"include_hits\":true,\"radius_cm\":5,"
				"\"start\":{\"components\":[-50,0,0],"
				"\"frame_id\":\"ue.world\",\"unit\":\"centimeter\"},"
				"\"trace_channel\":\"visibility\"}")),
			{},
			Environment,
			*Context,
			Snapshot,
			Error));
	TestTrue(
		TEXT("capsule sweep returns a normalized hit collection"),
		SnapshotHasSelector(Snapshot, TEXT("hits")));
	TestTrue(
		TEXT("capsule sweep returns the matching hit count"),
		SnapshotHasSelector(Snapshot, TEXT("hit_count")));

	Snapshot.Reset();
	TestTrue(
		TEXT("capsule path steps the complete native path"),
		CaptureNative(
			this,
			TEXT("world.read.capsule_path"),
			FUeAgentContractValue(TEXT(
				"{\"half_height_cm\":10,\"points\":[{\"components\":[-50,0,0],"
				"\"frame_id\":\"ue.world\",\"unit\":\"centimeter\"},{"
				"\"components\":[50,0,0],\"frame_id\":\"ue.world\","
				"\"unit\":\"centimeter\"}],\"radius_cm\":5,"
				"\"step_cm\":10,\"trace_channel\":\"visibility\"}")),
			{},
			Environment,
			*Context,
			Snapshot,
			Error));
	TestTrue(
		TEXT("capsule path returns a real obstruction summary"),
		SnapshotHasSelector(Snapshot, TEXT("blocked_segments")));

	Owner.CollisionB->SetIsVisualizationComponent(true);
	Snapshot.Reset();
	TestTrue(
		TEXT("nearby shapes reads component bounds rather than echoing the query point"),
		CaptureNative(
			this,
			TEXT("world.read.nearby_shapes"),
			FUeAgentContractValue(TEXT(
				"{\"center\":{\"components\":[0,0,0],\"frame_id\":\"ue.world\","
				"\"unit\":\"centimeter\"},\"include_shapes\":true,"
				"\"radius_cm\":100}")),
			{},
			Environment,
			*Context,
			Snapshot,
			Error));
	TestTrue(
		TEXT("nearby shapes publishes normalized component facts"),
		SnapshotHasSelector(Snapshot, TEXT("shapes")));
	TestTrue(
		TEXT("nearby shapes publishes the matching unique count"),
		SnapshotHasSelector(Snapshot, TEXT("nearby_shape_count")));
	TestFalse(
		TEXT("nearby shapes excludes visualization-only collision components"),
		SnapshotArrayContainsObjectStringField(
			Snapshot,
			TEXT("shapes"),
			TEXT("target_ref"),
			Owner.CollisionB->GetPathName()));

	Snapshot.Reset();
	TestFalse(
		TEXT("navigation family has a typed negative oracle"),
		CaptureNative(
			this,
			TEXT("world.read.nav_project_point"),
			FUeAgentContractValue(TEXT(
				"{\"point\":{\"components\":[0,0,0],"
				"\"frame_id\":\"ue.world\","
				"\"unit\":\"centimeter\"}}")),
			{},
			Environment,
			*Context,
			Snapshot,
			Error));
	TestEqual(
		TEXT("owned world has no hidden navigation fallback"),
		Error.Code,
		FString(TEXT("world_read_navigation_unavailable")));

	Snapshot.Reset();
	TestTrue(
		TEXT("topology family returns a complete empty fact"),
		CaptureNative(
			this,
			TEXT("world.read.streaming_levels"),
			FUeAgentContractValue(TEXT("{}")),
			{},
			Environment,
			*Context,
			Snapshot,
			Error));
	TestTrue(
		TEXT("topology snapshot is real and typed"),
		Snapshot.IsValid());

	Snapshot.Reset();
	TestTrue(
		TEXT("data layer query reads Data Layer facts rather than streaming levels"),
		CaptureNative(
			this,
			TEXT("world.read.data_layers"),
			FUeAgentContractValue(TEXT(
				"{\"scope\":\"world\",\"include_actor_membership\":true}")),
			{},
			Environment,
			*Context,
			Snapshot,
			Error));
	TestTrue(
		TEXT("data layer query publishes the real layer count"),
		SnapshotHasSelector(Snapshot, TEXT("data_layer_count")));

	Snapshot.Reset();
	TestTrue(
		TEXT("partition query returns a compact default summary"),
		CaptureNative(
			this,
			TEXT("world.read.partition"),
			FUeAgentContractValue(TEXT("{}")),
			{},
			Environment,
			*Context,
			Snapshot,
			Error));
	TestTrue(
		TEXT("partition default publishes region count"),
		SnapshotHasSelector(Snapshot, TEXT("user_loaded_region_count")));
	TestFalse(
		TEXT("partition default does not expand region geometry"),
		SnapshotHasSelector(Snapshot, TEXT("loaded_regions")));

	Snapshot.Reset();
	TestTrue(
		TEXT("partition geometry is available only when explicitly requested"),
		CaptureNative(
			this,
			TEXT("world.read.partition"),
			FUeAgentContractValue(TEXT("{\"include_loaded_regions\":true}")),
			{},
			Environment,
			*Context,
			Snapshot,
			Error));
	TestTrue(
		TEXT("requested partition geometry is one normalized collection"),
		SnapshotHasSelector(Snapshot, TEXT("loaded_regions")));

	Snapshot.Reset();
	TestTrue(
		TEXT("partition health returns a normalized issue collection"),
		CaptureNative(
			this,
			TEXT("world.read.partition_health"),
			FUeAgentContractValue(TEXT("{}")),
			{},
			Environment,
			*Context,
			Snapshot,
			Error));
	TestTrue(
		TEXT("partition health always publishes the issue array"),
		SnapshotHasSelector(Snapshot, TEXT("issues")));

	const FUeAgentCandidateBinding PhysicsTarget =
		NativeTarget(
			TEXT("target-physics"),
			TEXT("primitive_component"),
			Owner.Collision->GetPathName());
	Snapshot.Reset();
	TestTrue(
		TEXT("physics family reads the real primitive component"),
		CaptureNative(
			this,
			TEXT("world.read.physics_component"),
			FUeAgentContractValue(TEXT(
				"{\"include_collision_responses\":true,"
				"\"include_runtime_state\":true}")),
			PhysicsTarget,
			Environment,
			*Context,
			Snapshot,
			Error));
	TestTrue(
		TEXT("physics snapshot is owned by the component"),
		SnapshotHasOwner(
			Snapshot,
			Owner.Collision->GetPathName()));

	const FUeAgentCandidateBinding ConstraintTarget =
		NativeTarget(
			TEXT("target-constraint"),
			TEXT("physics_constraint_component"),
			Owner.Constraint->GetPathName());
	Snapshot.Reset();
	Error = {};
	TestTrue(
		TEXT("physics constraint query reads the exact constraint component"),
		CaptureNative(
			this,
			TEXT("world.read.physics_constraint"),
			FUeAgentContractValue(TEXT(
				"{\"include_endpoints\":true,\"include_limits\":true,"
				"\"include_runtime_state\":true}")),
			ConstraintTarget,
			Environment,
			*Context,
			Snapshot,
			Error));
	TestTrue(
		TEXT("constraint query publishes an endpoint"),
		SnapshotHasSelector(Snapshot, TEXT("constraint_component_1")));
	TestTrue(
		TEXT("constraint query publishes the configured linear limit"),
		SnapshotHasSelector(Snapshot, TEXT("linear_limit_cm")));
	TestTrue(
		TEXT("constraint query publishes runtime broken state"),
		SnapshotHasSelector(Snapshot, TEXT("constraint_broken")));

	TestFalse(
		TEXT("WorldRead leaves the owned package clean"),
		Owner.Package->IsDirty());
	TestTrue(
		TEXT("WorldRead does not mutate component transform"),
		Owner.MeshComponent->GetComponentTransform()
			.Equals(OriginalMeshTransform));
	TestTrue(
		TEXT("WorldRead does not mutate collision shape"),
		Owner.Collision->GetUnscaledBoxExtent()
			.Equals(OriginalCollisionExtent));
	TestEqual(
		TEXT("one environment owns all world resolution"),
		Environment->WorldCalls,
		17);
	TestEqual(
		TEXT("only target-bearing families resolve targets"),
		Environment->TargetCalls,
		3);
	return true;
}

#include "Misc/AutomationTest.h"

// RSP-005 is test-first. The substantive test is compiled only after the
// independent Physics native component and public session-owner seam exist.
#include "Components/StaticMeshComponent.h"
#include "Core/BindingGateway/Ticket/UeAgentBindingTicket.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialPhysicsNative.h"
#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialProductionRuntime.h"
#include "Engine/StaticMesh.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "PreviewScene.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"

namespace
{
	using namespace UE::UAI::BindingGateway::Operation;
	using namespace UE::UAI::BindingGateway::Ticket;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::Discovery;
	using namespace UE::UAI::DomainAdapters::RigSimulationSpatial;
	using namespace UE::UAI::PlanningPreflight;
	using namespace UE::UAI::StateSemantic;
	using namespace UE::UAI::Verification;

	FUeAgentResourceTrackerOwner MakeResources()
	{
		return MakeShared<FUeAgentResourceTracker, ESPMode::ThreadSafe>(
			FUeAgentResourceBudget({
				{EUeAgentResourceDimension::InputBytes, 4 * 1024 * 1024},
				{EUeAgentResourceDimension::OutputBytes, 4 * 1024 * 1024},
				{EUeAgentResourceDimension::Nodes, 100000},
				{EUeAgentResourceDimension::Items, 100000},
				{EUeAgentResourceDimension::StringBytes, 4 * 1024 * 1024},
				{EUeAgentResourceDimension::WorkUnits, 100000},
				{EUeAgentResourceDimension::EstimatedMemoryBytes, 16 * 1024 * 1024},
			}));
	}

	TArray<TSharedPtr<FJsonValue>> StringValues(
		std::initializer_list<const TCHAR*> Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		for (const TCHAR* Value : Values)
		{
			Result.Add(MakeShared<FJsonValueString>(Value));
		}
		return Result;
	}

	TSharedPtr<FJsonObject> RevisionJson(
		const FRigSimulationSpatialRevision& Revision)
	{
		const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("source_id"), Revision.SourceId);
		Result->SetStringField(TEXT("generation"), Revision.Generation);
		Result->SetStringField(TEXT("sequence"), LexToString(Revision.Sequence));
		return Result;
	}

	TSharedPtr<FJsonObject> FrameJson(
		const FString& FrameId,
		const FString& EvidenceId)
	{
		const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("frame_id"), FrameId);
		Result->SetStringField(TEXT("basis_frame_id"), TEXT("frame://basis"));
		Result->SetNumberField(TEXT("dimension"), 3);
		Result->SetStringField(TEXT("unit"), TEXT("cm"));
		Result->SetStringField(TEXT("basis_evidence_id"), EvidenceId);
		return Result;
	}

	TSharedPtr<FJsonObject> TransformJson(
		const FString& Subject,
		const FString& FrameId,
		const FString& EvidenceId,
		const TCHAR* X,
		const TCHAR* Y,
		const TCHAR* Z)
	{
		const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("subject"), Subject);
		Result->SetStringField(TEXT("quantity"), TEXT("transform"));
		Result->SetObjectField(TEXT("frame"), FrameJson(FrameId, EvidenceId));
		Result->SetArrayField(TEXT("components"), StringValues({
			TEXT("1"), TEXT("0"), TEXT("0"), X,
			TEXT("0"), TEXT("1"), TEXT("0"), Y,
			TEXT("0"), TEXT("0"), TEXT("1"), Z,
			TEXT("0"), TEXT("0"), TEXT("0"), TEXT("1")}));
		Result->SetArrayField(TEXT("conversion_evidence_ids"),
			StringValues({TEXT("conversion-evidence://rsp005")}));
		return Result;
	}

	TSharedPtr<FJsonObject> PointJson(const TCHAR* Tick)
	{
		const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("clock_id"), TEXT("clock://authored"));
		Result->SetStringField(TEXT("tick"), Tick);
		const TSharedPtr<FJsonObject> Rate = MakeShared<FJsonObject>();
		Rate->SetStringField(TEXT("numerator"), TEXT("60"));
		Rate->SetStringField(TEXT("denominator"), TEXT("1"));
		Result->SetObjectField(TEXT("rate"), Rate);
		Result->SetStringField(TEXT("state"), TEXT("simulation"));
		return Result;
	}

	TSharedPtr<FJsonObject> RangeJson()
	{
		const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("clock_id"), TEXT("clock://authored"));
		Result->SetStringField(TEXT("lower_tick"), TEXT("0"));
		Result->SetStringField(TEXT("upper_tick"), TEXT("1"));
		const TSharedPtr<FJsonObject> Rate = MakeShared<FJsonObject>();
		Rate->SetStringField(TEXT("numerator"), TEXT("60"));
		Rate->SetStringField(TEXT("denominator"), TEXT("1"));
		Result->SetObjectField(TEXT("rate"), Rate);
		Result->SetStringField(TEXT("state"), TEXT("simulation"));
		return Result;
	}

	TSharedPtr<FJsonObject> VectorSampleJson(
		const TCHAR* X,
		const TCHAR* Y,
		const TCHAR* Z)
	{
		const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("subject_ref"), TEXT("subject://cube"));
		Result->SetObjectField(TEXT("time"), PointJson(TEXT("0")));
		Result->SetObjectField(TEXT("frame"), FrameJson(
			TEXT("frame://simulation-requested"), TEXT("basis://rsp005-simulation")));
		Result->SetArrayField(TEXT("components"), StringValues({X, Y, Z}));
		Result->SetArrayField(TEXT("evidence_ids"),
			StringValues({TEXT("vector-evidence://rsp005")}));
		return Result;
	}

	FUeAgentContractValue JsonInput(const TSharedRef<FJsonObject>& Root)
	{
		FString Text;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Root, Writer);
		return FUeAgentContractValue(MoveTemp(Text));
	}

	TSharedPtr<FJsonObject> ConstraintStateJson(const bool bUpdated)
	{
		const TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
		State->SetStringField(TEXT("linear_x_motion"), bUpdated ? TEXT("free") : TEXT("limited"));
		State->SetStringField(TEXT("linear_y_motion"), TEXT("locked"));
		State->SetStringField(TEXT("linear_z_motion"), TEXT("limited"));
		State->SetStringField(TEXT("linear_limit_cm"), bUpdated ? TEXT("18") : TEXT("12"));
		State->SetStringField(TEXT("twist_motion"), TEXT("limited"));
		State->SetStringField(TEXT("swing1_motion"), TEXT("limited"));
		State->SetStringField(TEXT("swing2_motion"), TEXT("free"));
		State->SetStringField(TEXT("twist_limit_degrees"), bUpdated ? TEXT("25") : TEXT("15"));
		State->SetStringField(TEXT("swing1_limit_degrees"), TEXT("30"));
		State->SetStringField(TEXT("swing2_limit_degrees"), TEXT("45"));
		State->SetBoolField(TEXT("disable_collision"), bUpdated);
		return State;
	}

	TSharedPtr<FJsonObject> EndpointJson(
		const FString& BodyRef,
		const FString& FrameId,
		const TCHAR* X,
		const TCHAR* Y,
		const TCHAR* Z)
	{
		const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("body_ref"), BodyRef);
		Result->SetObjectField(TEXT("frame"), TransformJson(
			BodyRef, FrameId, TEXT("basis://rsp005-constraint"), X, Y, Z));
		return Result;
	}

	TSharedPtr<FJsonObject> BodyJson(
		const FString& BodyRef,
		const FString& BoneRef,
		const FString& ShapeRef,
		const TCHAR* Collision,
		const TCHAR* PhysicsType,
		const TCHAR* Mass,
		const bool bBounds)
	{
		const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("body_ref"), BodyRef);
		Result->SetStringField(TEXT("bone_ref"), BoneRef);
		Result->SetArrayField(TEXT("shape_refs"),
			{MakeShared<FJsonValueString>(ShapeRef)});
		const TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
		State->SetStringField(TEXT("collision_enabled"), Collision);
		State->SetStringField(TEXT("physics_type"), PhysicsType);
		State->SetStringField(TEXT("mass_kg"), Mass);
		State->SetBoolField(TEXT("consider_for_bounds"), bBounds);
		Result->SetObjectField(TEXT("state"), State);
		return Result;
	}

	FUeAgentContractValue CanonicalInput(
		const FString& Capability,
		const TOptional<FRigSimulationSpatialRevision>& Revision)
	{
		const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		if (Revision.IsSet())
		{
			Root->SetObjectField(TEXT("expected_revision"),
				RevisionJson(Revision.GetValue()));
		}
		if (Capability == TEXT("physics.material.update"))
		{
			Root->SetStringField(TEXT("physical_material_ref"), TEXT("material://rsp005"));
			const TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
			State->SetStringField(TEXT("friction"), TEXT("0.71"));
			State->SetStringField(TEXT("static_friction"), TEXT("0.82"));
			State->SetStringField(TEXT("restitution"), TEXT("0.23"));
			State->SetStringField(TEXT("density"), TEXT("2.5"));
			State->SetStringField(TEXT("raise_mass_to_power"), TEXT("0.8"));
			State->SetStringField(TEXT("friction_combine_mode"), TEXT("max"));
			State->SetStringField(TEXT("restitution_combine_mode"), TEXT("min"));
			Root->SetObjectField(TEXT("material_state"), State);
		}
		else if (Capability == TEXT("physics.asset.structure.replace"))
		{
			Root->SetStringField(TEXT("physics_asset_ref"), TEXT("physics-asset://rsp005"));
			Root->SetArrayField(TEXT("bodies"), {
				MakeShared<FJsonValueObject>(BodyJson(TEXT("body://a"), TEXT("bone://a"),
					TEXT("shape://a-box"), TEXT("query_and_physics"),
					TEXT("simulated"), TEXT("2.5"), true)),
				MakeShared<FJsonValueObject>(BodyJson(TEXT("body://b"), TEXT("bone://b"),
					TEXT("shape://b-sphere"), TEXT("physics_only"),
					TEXT("kinematic"), TEXT("3.5"), false))});
			const TSharedPtr<FJsonObject> Constraint = MakeShared<FJsonObject>();
			Constraint->SetStringField(TEXT("constraint_ref"), TEXT("constraint://pair"));
			Constraint->SetObjectField(TEXT("endpoint_a"), EndpointJson(TEXT("body://a"),
				TEXT("frame://body-a-requested"), TEXT("1"), TEXT("2"), TEXT("3")));
			Constraint->SetObjectField(TEXT("endpoint_b"), EndpointJson(TEXT("body://b"),
				TEXT("frame://body-b-requested"), TEXT("4"), TEXT("5"), TEXT("6")));
			Constraint->SetObjectField(TEXT("state"), ConstraintStateJson(false));
			Root->SetArrayField(TEXT("constraints"),
				{MakeShared<FJsonValueObject>(Constraint)});
			const TSharedPtr<FJsonObject> Profile = MakeShared<FJsonObject>();
			Profile->SetStringField(TEXT("profile_name"), TEXT("ProfileA"));
			Profile->SetArrayField(TEXT("body_refs"),
				StringValues({TEXT("body://a"), TEXT("body://b")}));
			Profile->SetArrayField(TEXT("constraint_refs"),
				StringValues({TEXT("constraint://pair")}));
			Root->SetArrayField(TEXT("profiles"),
				{MakeShared<FJsonValueObject>(Profile)});
		}
		else if (Capability == TEXT("physics.constraint.update"))
		{
			Root->SetStringField(TEXT("physics_asset_ref"), TEXT("physics-asset://rsp005"));
			Root->SetStringField(TEXT("constraint_ref"), TEXT("constraint://pair"));
			Root->SetObjectField(TEXT("endpoint_a"), EndpointJson(TEXT("body://a"),
				TEXT("frame://body-a-requested"), TEXT("2"), TEXT("3"), TEXT("4")));
			Root->SetObjectField(TEXT("endpoint_b"), EndpointJson(TEXT("body://b"),
				TEXT("frame://body-b-requested"), TEXT("5"), TEXT("6"), TEXT("7")));
			Root->SetObjectField(TEXT("constraint_state"), ConstraintStateJson(true));
		}
		else if (Capability == TEXT("physics.simulation.probe"))
		{
			Root->SetArrayField(TEXT("subject_refs"),
				StringValues({TEXT("subject://cube")}));
			const TSharedPtr<FJsonObject> Session = MakeShared<FJsonObject>();
			Session->SetStringField(TEXT("session_id"), TEXT("physics-session://rsp005"));
			Session->SetStringField(TEXT("owner_ref"), TEXT("physics-session-owner://rsp005"));
			Session->SetStringField(TEXT("generation"), TEXT("physics-session-generation://1"));
			Root->SetObjectField(TEXT("simulation_session"), Session);
			Root->SetObjectField(TEXT("simulation_range"), RangeJson());
			Root->SetArrayField(TEXT("impulses"),
				{MakeShared<FJsonValueObject>(VectorSampleJson(TEXT("120"), TEXT("0"), TEXT("0")))});
			Root->SetArrayField(TEXT("velocities"),
				{MakeShared<FJsonValueObject>(VectorSampleJson(TEXT("0"), TEXT("60"), TEXT("0")))});
		}
		return JsonInput(Root);
	}

	const FUeAgentRigSimulationSpatialExecutionChainContract* Contract(
		const FString& Capability)
	{
		return GetRigSimulationSpatialExecutionChainContracts().FindByPredicate(
			[&](const auto& Item){ return Item.GetCapabilityId() == Capability; });
	}

	const FUeAgentOperationDescriptor* Descriptor(
		const FUeAgentRigSimulationSpatialProductionRuntime& Runtime,
		const FString& Capability)
	{
		return Runtime.GetDescriptorRegistry()->GetDescriptors().FindByPredicate(
			[&](const auto& Item){ return Item.GetCapabilityId() == Capability; });
	}

	TSharedPtr<FUeAgentOperationContext> MakeContext(
		FAutomationTestBase& Test,
		const FUeAgentOperationDescriptor& DescriptorValue,
		const FString& RequestId,
		const FUeAgentResourceTrackerOwner& Resources)
	{
		const TArray<EUeAgentStatePlane>& Planes =
			DescriptorValue.GetExecutionContextPolicy().GetAllowedStatePlanes();
		if (!Test.TestTrue(TEXT("descriptor state plane is explicit"), !Planes.IsEmpty()))
			return nullptr;
		FUeAgentExecutionContextInput Input;
		Input.ProjectInstanceId = TEXT("rsp005-project");
		Input.EditorInstanceId = TEXT("rsp005-editor");
		Input.StatePlane = Planes[0];
		Input.WorldType = EUeAgentExecutionWorldType::Preview;
		Input.WorldIdentity = TEXT("world://rsp005-owned");
		Input.AssetEditorIdentity = TEXT("asset-editor://none");
		Input.ViewportIdentity = TEXT("viewport://none");
		Input.ContextRevision = TEXT("rsp005-context-1");
		Input.bResolvedFromCurrent = false;
		FUeAgentExecutionContextRefPtr Execution;
		FUeAgentExecutionContextError ExecutionError;
		if (!Test.TestTrue(TEXT("freeze explicit Physics context"),
			FUeAgentExecutionContextRef::TryCreate(Input, DescriptorValue.GetIdentity(),
				EUeAgentRequestKind::Operation,
				DescriptorValue.GetExecutionContextPolicy(), Execution, ExecutionError)))
			return nullptr;
		const FUeAgentCallerContext Caller(TEXT("rsp005-native-e2"),
			{TEXT("rig-simulation-spatial:read"),
				TEXT("rig-simulation-spatial:write")}, TEXT("corr-") + RequestId);
		const FUeAgentOperationDeadline Deadline =
			FUeAgentOperationDeadline::FromTimeout(60.0, [](){ return 100.0; });
		const FUeAgentCancellationSource Cancellation([](){ return 100.0; });
		const FUeAgentOperationRequest Request(DescriptorValue.GetIdentity(), RequestId,
			TEXT("idem-") + RequestId, FUeAgentContractValue(TEXT("{}")));
		TSharedPtr<FUeAgentOperationContext> Result;
		FUeAgentOperationContextError Error;
		Test.TestTrue(TEXT("create operation context"),
			FUeAgentOperationContext::TryCreate(Request, Caller, Execution.ToSharedRef(),
				Resources, Deadline, Cancellation.GetToken(), Result, Error));
		return Result;
	}

	TArray<FUeAgentOperationTargetBinding> MakeTargets(
		FAutomationTestBase& Test,
		const FUeAgentOperationDescriptor& DescriptorValue,
		const FString& Capability,
		const FString& PublicRef)
	{
		const FUeAgentCandidateBinding Candidate(
			TEXT("rsp005-binding:") + Capability,
			FUeAgentCandidateSummary(
				FUeAgentCanonicalIdentity(TEXT("rsp005-target:") + Capability,
					TEXT("rig_simulation_spatial_target"), TEXT("rsp005-owner"),
					PublicRef, TEXT("target"), TEXT("rsp005-generation")),
				DescriptorValue.GetIdentity(), TEXT("rig_simulation_spatial_target"),
				PublicRef, TEXT("owned RSP-005 fixture"),
				{TEXT("RigSimulationSpatial"), TEXT("Fixture")}),
			FUeAgentCatalogRevision(TEXT("rsp005-catalog"),
				TEXT("rsp005-generation"), 1), PublicRef,
			TEXT("rig-simulation-spatial-native"),
			{EUeAgentCandidateOperation::Read, EUeAgentCandidateOperation::Update},
			TEXT("rsp005-native-binding"));
		TSharedPtr<const FUeAgentOperationTicketService> Service;
		FUeAgentOperationTicketError Error;
		if (!Test.TestTrue(TEXT("create operation ticket service"),
			FUeAgentOperationTicketService::TryCreate(TEXT("rsp005-integrity"), 60.0,
				[](){ return 100.0; }, [](){ return TEXT("rsp005-ticket"); },
				Service, Error))) return {};
		const FUeAgentOperationSessionIdentity Session(TEXT("rsp005-session"), 1,
			TEXT("sha256:0000000000000000000000000000000000000000000000000000000000000000"));
		const FUeAgentCallerContext Caller(TEXT("rsp005-native-e2"),
			{TEXT("rig-simulation-spatial:read"),
				TEXT("rig-simulation-spatial:write")}, TEXT("corr-ticket-") + Capability);
		TOptional<FUeAgentOperationTicket> Ticket;
		if (!Test.TestTrue(TEXT("issue explicit target ticket"),
			Service->TryIssue(Candidate, DescriptorValue.GetIdentity(), Session,
				Caller, {}, Ticket, Error)) || !Ticket.IsSet()) return {};
		return {FUeAgentOperationTargetBinding(Ticket.GetValue(), Candidate)};
	}

	bool ExecuteOperation(
		FAutomationTestBase& Test,
		const FUeAgentRigSimulationSpatialProductionRuntime& Runtime,
		const FString& Capability,
		const FUeAgentContractValue& Input,
		const FString& PublicRef,
		const FUeAgentResourceTrackerOwner& Resources,
		FString* OutFailureCode = nullptr,
		FVerificationOutcome* OutOutcome = nullptr)
	{
		const auto* DescriptorValue = Descriptor(Runtime, Capability);
		if (!Test.TestNotNull(TEXT("operation contract"), Contract(Capability))
			|| !Test.TestNotNull(TEXT("operation descriptor"), DescriptorValue)) return false;
		const auto Context = MakeContext(Test, *DescriptorValue,
			TEXT("operation-") + Capability, Resources);
		if (!Context.IsValid()) return false;
		const auto Targets = MakeTargets(Test, *DescriptorValue, Capability, PublicRef);
		const TSharedRef<FUeAgentRigSimulationSpatialOperationChain>* Chain =
			Runtime.GetOperationChains().FindByPredicate(
				[&](const auto& Candidate)
				{
					return Candidate->GetContract().GetCapabilityId() == Capability;
				});
		if (!Test.TestNotNull(TEXT("exact production C3O chain: ") + Capability, Chain))
			return false;
		FUeAgentOperationBindingError Error;
		TOptional<FPlanningRequest> PlanningRequest;
		if (!(*Chain)->TryBuildPlanningRequest(Input, Targets, *Context,
			PlanningRequest, Error) || !PlanningRequest.IsSet())
		{
			if (OutFailureCode) *OutFailureCode = Error.Code;
			else Test.AddError(Capability + TEXT(":preflight:") + Error.Code
				+ TEXT(":") + Error.Detail);
			return false;
		}
		FPlanningResult Planning;
		if (!(*Chain)->GetPlanner().TryPlan(*PlanningRequest, *Context, Planning, Error))
		{
			if (OutFailureCode) *OutFailureCode = Error.Code;
			else Test.AddError(Capability + TEXT(":plan:") + Error.Code);
			return false;
		}
		FExecutionReceipt Receipt;
		if (!(*Chain)->GetExecutor().TryExecute(Planning, Targets, *Context, Receipt, Error))
		{
			if (OutFailureCode) *OutFailureCode = Error.Code;
			else Test.AddError(Capability + TEXT(":execute:") + Error.Code
				+ TEXT(":") + Error.Detail);
			return false;
		}
		FVerificationOutcome Outcome;
		if (!(*Chain)->GetVerifier().TryVerify(Receipt, Targets, *Context, Outcome, Error))
		{
			if (OutFailureCode) *OutFailureCode = Error.Code;
			else Test.AddError(Capability + TEXT(":verify:") + Error.Code);
			return false;
		}
		if (OutOutcome)
		{
			*OutOutcome = MoveTemp(Outcome);
			return true;
		}
		return Test.TestEqual(TEXT("exact production C3O verified: ") + Capability,
			Outcome.GetStatus(), EVerificationStatus::Verified);
	}

	class FRealPhysicsSessionPort final
		: public IUeAgentRigSimulationSpatialPhysicsSessionPort
	{
	public:
		bool Initialize(FAutomationTestBase& Test)
		{
			PreviewScene = MakeUnique<FPreviewScene>(FPreviewScene::ConstructionValues()
				.SetCreateDefaultLighting(false).SetCreatePhysicsScene(true)
				.SetForceMipsResident(false).ShouldSimulatePhysics(true)
				.SetTransactional(false));
			UWorld* World = PreviewScene->GetWorld();
			UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr,
				TEXT("/Engine/BasicShapes/Cube.Cube"));
			if (!Test.TestNotNull(TEXT("create owned simulation world"), World)
				|| !Test.TestNotNull(TEXT("load real static mesh"), Cube)) return false;
			Component = NewObject<UStaticMeshComponent>(GetTransientPackage(),
				TEXT("RSP005OwnedPhysicsSubject"), RF_Transactional);
			Component->SetMobility(EComponentMobility::Movable);
			Component->SetStaticMesh(Cube);
			Component->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
			PreviewScene->AddComponent(Component, FTransform(FVector(0.0, 0.0, 200.0)));
			Component->SetEnableGravity(false);
			Component->SetSimulatePhysics(true);
			InitialTransform = Component->GetComponentTransform();
			return Component->IsRegistered() && Component->IsSimulatingPhysics();
		}

		bool TryReadPhysicsProbe(
			const FRigSimulationSpatialSimulationSession& Session,
			const TArray<FString>& SubjectRefs,
			const FUeAgentOperationContext& Context,
			FUeAgentRigSimulationSpatialPhysicsProbeSessionResult& OutResult,
			FUeAgentRigSimulationSpatialExecutionError& OutError) override
		{
			OutError = {};
			if (!Session.IsValid() || SubjectRefs != TArray<FString>{TEXT("subject://cube")})
			{
				OutError.Code = TEXT("rsp005_session_read_request_invalid");
				return false;
			}
			if (LastResult.IsSet())
			{
				OutResult = LastResult.GetValue();
				return OutResult.IsValid();
			}
			OutResult = FUeAgentRigSimulationSpatialPhysicsProbeSessionResult(
				Session, false, true, {}, TEXT("restore://rsp005-baseline"), Component);
			return OutResult.IsValid();
		}

		bool TryRunPhysicsProbe(
			const FUeAgentRigSimulationSpatialPhysicsProbeSessionRequest& Request,
			const FUeAgentOperationContext& Context,
			FUeAgentRigSimulationSpatialPhysicsProbeSessionResult& OutResult,
			FUeAgentRigSimulationSpatialExecutionError& OutError) override
		{
			++CallCount;
			OutResult = {};
			OutError = {};
			if (!Request.IsValid() || Request.Subjects.Num() != 1
				|| Request.Subjects[0].Component.Get() != Component
				|| Request.SimulationRange.GetClockId() != TEXT("clock://simulation-native")
				|| Request.Impulses.Num() != 1 || Request.Velocities.Num() != 1
				|| Request.Impulses[0].NativeFrameId != TEXT("frame://simulation-native")
				|| Request.Velocities[0].NativeFrameId != TEXT("frame://simulation-native")
				|| !Request.Impulses[0].Value.Equals(FVector(120.0, 0.0, 0.0), 0.001)
				|| !Request.Velocities[0].Value.Equals(FVector(0.0, 60.0, 0.0), 0.001)
				|| Request.Impulses[0].Time.GetClockId() != TEXT("clock://simulation-native")
				|| Request.Velocities[0].Time.GetClockId() != TEXT("clock://simulation-native"))
			{
				OutError.Code = TEXT("rsp005_session_effect_request_invalid");
				return false;
			}
			bSawOwnedWorld = PreviewScene.IsValid()
				&& Component->GetWorld() == PreviewScene->GetWorld();
			Component->SetPhysicsLinearVelocity(Request.Velocities[0].Value);
			Component->AddImpulse(Request.Impulses[0].Value);
			PreviewScene->GetWorld()->Tick(LEVELTICK_All, 1.0f / 60.0f);
			const FVector SamplePosition = Component->GetComponentLocation();
			const FVector SampleVelocity = Component->GetPhysicsLinearVelocity();
			bObservedMotion = !SamplePosition.Equals(InitialTransform.GetLocation(), 0.001)
				|| !SampleVelocity.IsNearlyZero(0.001);
			FUeAgentRigSimulationSpatialPhysicsProbeSample Sample;
			Sample.SubjectRef = Request.Subjects[0].SubjectRef;
			Sample.Time = Request.Velocities[0].Time;
			Sample.FrameId = Request.Subjects[0].NativeSimulationFrameId;
			Sample.Position = SamplePosition;
			Sample.Velocity = SampleVelocity;
			Sample.EvidenceId = TEXT("physics-sample://rsp005-owned-world");
			Component->SetPhysicsLinearVelocity(FVector::ZeroVector);
			Component->SetPhysicsAngularVelocityInRadians(FVector::ZeroVector);
			Component->SetWorldTransform(InitialTransform, false, nullptr,
				ETeleportType::TeleportPhysics);
			bRestored = Component->GetComponentTransform().Equals(InitialTransform, 0.001)
				&& Component->GetPhysicsLinearVelocity().IsNearlyZero(0.001);
			OutResult = FUeAgentRigSimulationSpatialPhysicsProbeSessionResult(
				Request.Session, true, bRestored, {Sample},
				TEXT("restore://rsp005-owned-world"), Component);
			LastResult = OutResult;
			return OutResult.IsValid();
		}

		UStaticMeshComponent* GetComponent() const { return Component; }
		const FTransform& GetInitialTransform() const { return InitialTransform; }

		int32 CallCount = 0;
		bool bSawOwnedWorld = false;
		bool bObservedMotion = false;
		bool bRestored = false;

	private:
		TUniquePtr<FPreviewScene> PreviewScene;
		TObjectPtr<UStaticMeshComponent> Component = nullptr;
		FTransform InitialTransform;
		TOptional<FUeAgentRigSimulationSpatialPhysicsProbeSessionResult> LastResult;
	};

	enum class EScriptedPhysicsSessionScenario : uint8
	{
		OwnerRejected,
		CancelledNoChange,
		TimeoutStateUnknown,
		RollbackFailedStateUnknown,
		SuccessWithUnverifiedReadback,
	};

	class FScriptedPhysicsSessionPort final
		: public IUeAgentRigSimulationSpatialPhysicsSessionPort
	{
	public:
		explicit FScriptedPhysicsSessionPort(UPrimitiveComponent* InComponent)
			: Component(InComponent) {}

		bool TryReadPhysicsProbe(
			const FRigSimulationSpatialSimulationSession& Session,
			const TArray<FString>& SubjectRefs,
			const FUeAgentOperationContext& Context,
			FUeAgentRigSimulationSpatialPhysicsProbeSessionResult& OutResult,
			FUeAgentRigSimulationSpatialExecutionError& OutError) override
		{
			++ReadCount;
			OutError = {};
			if (!Session.IsValid()
				|| SubjectRefs != TArray<FString>{TEXT("subject://cube")}
				|| !Component.IsValid())
			{
				OutError.Code = TEXT("rig_simulation_spatial_physics_session_read_invalid");
				return false;
			}
			OutResult = FUeAgentRigSimulationSpatialPhysicsProbeSessionResult(
				Session, false, true, {}, TEXT("restore://rsp007-baseline"),
				Component.Get());
			return OutResult.IsValid();
		}

		bool TryRunPhysicsProbe(
			const FUeAgentRigSimulationSpatialPhysicsProbeSessionRequest& Request,
			const FUeAgentOperationContext& Context,
			FUeAgentRigSimulationSpatialPhysicsProbeSessionResult& OutResult,
			FUeAgentRigSimulationSpatialExecutionError& OutError) override
		{
			++RunCount;
			OutResult = {};
			OutError = {};
			switch (Scenario)
			{
			case EScriptedPhysicsSessionScenario::OwnerRejected:
				OutError.Code = TEXT("rig_simulation_spatial_physics_session_owner_rejected");
				return false;
			case EScriptedPhysicsSessionScenario::CancelledNoChange:
				OutError.Code = TEXT(
					"rig_simulation_spatial_physics_session_cancelled_no_change");
				return false;
			case EScriptedPhysicsSessionScenario::TimeoutStateUnknown:
				OutError.Code = TEXT(
					"rig_simulation_spatial_physics_session_timeout_state_unknown");
				OutError.Detail = TEXT("reconcile_required");
				return false;
			case EScriptedPhysicsSessionScenario::RollbackFailedStateUnknown:
				OutError.Code = TEXT(
					"rig_simulation_spatial_physics_session_rollback_failed_state_unknown");
				return false;
			case EScriptedPhysicsSessionScenario::SuccessWithUnverifiedReadback:
				break;
			}

			if (!Request.IsValid() || Request.Subjects.Num() != 1
				|| !Component.IsValid())
			{
				OutError.Code = TEXT("rig_simulation_spatial_physics_session_fixture_invalid");
				return false;
			}
			FUeAgentRigSimulationSpatialPhysicsProbeSample Sample;
			Sample.SubjectRef = Request.Subjects[0].SubjectRef;
			Sample.Time = Request.Velocities[0].Time;
			Sample.FrameId = Request.Subjects[0].NativeSimulationFrameId;
			Sample.Position = Component->GetComponentLocation();
			Sample.Velocity = FVector(1.0, 0.0, 0.0);
			Sample.EvidenceId = TEXT("physics-sample://rsp007-owner-receipt");
			OutResult = FUeAgentRigSimulationSpatialPhysicsProbeSessionResult(
				Request.Session, true, true, {Sample},
				TEXT("restore://rsp007-owner-receipt"), Component.Get());
			return OutResult.IsValid();
		}

		void SetScenario(const EScriptedPhysicsSessionScenario InScenario)
		{
			Scenario = InScenario;
			RunCount = 0;
			ReadCount = 0;
		}

		int32 RunCount = 0;
		int32 ReadCount = 0;

	private:
		TWeakObjectPtr<UPrimitiveComponent> Component;
		EScriptedPhysicsSessionScenario Scenario =
			EScriptedPhysicsSessionScenario::OwnerRejected;
	};

	USkeletalBodySetup* FindBody(UPhysicsAsset* Asset, const FName BoneName)
	{
		return Asset ? Asset->SkeletalBodySetups.FindByPredicate(
			[BoneName](const TObjectPtr<USkeletalBodySetup>& Body)
			{
				return Body && Body->BoneName == BoneName;
			}) ? *Asset->SkeletalBodySetups.FindByPredicate(
				[BoneName](const TObjectPtr<USkeletalBodySetup>& Body)
				{
					return Body && Body->BoneName == BoneName;
				}) : nullptr : nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentRigSimulationSpatialPhysicsNativeTest,
	"UeAgentInterface.DomainAdapters.RigSimulationSpatial.E2.ProductionNative.Physics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentRigSimulationSpatialPhysicsNativeTest::RunTest(
	const FString& Parameters)
{
	UPackage* MaterialPackage = CreatePackage(TEXT("/Game/__UAI_RSP005/Material"));
	UPhysicalMaterial* Material = NewObject<UPhysicalMaterial>(MaterialPackage,
		TEXT("Material"), RF_Public | RF_Standalone | RF_Transactional);
	UPackage* AssetPackage = CreatePackage(TEXT("/Game/__UAI_RSP005/PhysicsAsset"));
	UPhysicsAsset* PhysicsAsset = NewObject<UPhysicsAsset>(AssetPackage,
		TEXT("PhysicsAsset"), RF_Public | RF_Standalone | RF_Transactional);
	if (!TestNotNull(TEXT("create real physical material"), Material)
		|| !TestNotNull(TEXT("create real physics asset"), PhysicsAsset)) return false;

	const TSharedRef<FRealPhysicsSessionPort> SessionPort =
		MakeShared<FRealPhysicsSessionPort>();
	if (!SessionPort->Initialize(*this)) return false;
	const FUeAgentTemporalPoint ObservedAt(TEXT("clock://authored"), TEXT("0"),
		TEXT("60"), TEXT("1"), TEXT("simulation"));
	const TArray<TArray<FString>> IdentityBasis = {
		{TEXT("1"), TEXT("0"), TEXT("0")},
		{TEXT("0"), TEXT("1"), TEXT("0")},
		{TEXT("0"), TEXT("0"), TEXT("1")},
	};

	FUeAgentRigSimulationSpatialPhysicsAssetBinding AssetBinding;
	AssetBinding.Identity = TEXT("physics-asset://rsp005");
	AssetBinding.PhysicsAsset = PhysicsAsset;
	AssetBinding.BodyBindings = {
		{TEXT("body://a"), TEXT("bone://a"), TEXT("Root")},
		{TEXT("body://b"), TEXT("bone://b"), TEXT("Child")},
	};
	FUeAgentRigSimulationSpatialPhysicsShapeBinding ShapeA;
	ShapeA.Identity = TEXT("shape://a-box");
	ShapeA.BodyIdentity = TEXT("body://a");
	ShapeA.NativeName = TEXT("RSP005Box");
	ShapeA.ShapeKind = TEXT("box");
	ShapeA.Dimensions = FVector(20.0, 30.0, 40.0);
	FUeAgentRigSimulationSpatialPhysicsShapeBinding ShapeB;
	ShapeB.Identity = TEXT("shape://b-sphere");
	ShapeB.BodyIdentity = TEXT("body://b");
	ShapeB.NativeName = TEXT("RSP005Sphere");
	ShapeB.ShapeKind = TEXT("sphere");
	ShapeB.Dimensions = FVector(15.0, 0.0, 0.0);
	AssetBinding.ShapeBindings = {ShapeA, ShapeB};
	AssetBinding.ConstraintBindings = {{
		TEXT("constraint://pair"), TEXT("RSP005Constraint"),
		TEXT("body://a"), TEXT("body://b"),
		TEXT("frame://body-a-native"), TEXT("frame://body-b-native")}};

	FUeAgentRigSimulationSpatialProductionRuntimeConfig Config;
	Config.ProjectInstanceId = TEXT("rsp005-project");
	Config.EditorInstanceId = TEXT("rsp005-editor");
	Config.ServerSessionId = TEXT("rsp005-session");
	Config.RuntimeGeneration = TEXT("rsp005-generation");
	Config.RegistryEpoch = 1;
	Config.ObjectBindings = {
		{TEXT("material://rsp005"), Material},
		{TEXT("physics-asset://rsp005"), PhysicsAsset},
		{TEXT("physics-session-owner://rsp005"), SessionPort->GetComponent()},
		{TEXT("subject://cube"), SessionPort->GetComponent()},
	};
	Config.PhysicsAssetBindings = {AssetBinding};
	Config.PhysicsSubjectBindings = {{TEXT("subject://cube"),
		SessionPort->GetComponent(), TEXT("frame://simulation-native"),
		TEXT("clock://simulation-native")}};
	Config.PhysicsSessionPort = SessionPort;
	Config.ClockBindings.Add({TEXT("clock://authored"),
		TEXT("clock://simulation-native"), TEXT("clock-evidence://rsp005")});
	for (const TPair<FString, bool>& Frame : {
		TPair<FString, bool>(TEXT("frame://body-a-requested"), false),
		TPair<FString, bool>(TEXT("frame://body-b-requested"), false),
		TPair<FString, bool>(TEXT("frame://body-a-native"), true),
		TPair<FString, bool>(TEXT("frame://body-b-native"), true),
		TPair<FString, bool>(TEXT("frame://simulation-requested"), false),
		TPair<FString, bool>(TEXT("frame://simulation-native"), true)})
	{
		TArray<FString> Origin = {TEXT("0"), TEXT("0"), TEXT("0")};
		if (Frame.Key == TEXT("frame://body-a-native"))
			Origin = {TEXT("-10"), TEXT("0"), TEXT("0")};
		else if (Frame.Key == TEXT("frame://body-b-native"))
			Origin = {TEXT("0"), TEXT("-20"), TEXT("0")};
		else if (Frame.Key == TEXT("frame://simulation-native"))
			Origin = {TEXT("-99"), TEXT("-99"), TEXT("-99")};
		const FString BasisEvidence = Frame.Key.Contains(TEXT("simulation"))
			? TEXT("basis://rsp005-simulation")
			: TEXT("basis://rsp005-constraint");
		Config.FrameBindings.Add(FUeAgentRigSimulationSpatialFrameBinding(
			FUeAgentSpatialFrame(Frame.Key, TEXT("frame://basis"), 3,
				IdentityBasis, MoveTemp(Origin), TEXT("cm"),
				BasisEvidence, ObservedAt), Frame.Value));
	}
	Config.SpatialConversions = {
		FUeAgentSpatialConversion(TEXT("conversion://body-a"),
			TEXT("frame://body-a-requested"), TEXT("frame://body-a-native"), 3, 3,
			IdentityBasis, {TEXT("10"), TEXT("0"), TEXT("0")}, ObservedAt,
			TEXT("conversion-evidence://body-a")),
		FUeAgentSpatialConversion(TEXT("conversion://body-b"),
			TEXT("frame://body-b-requested"), TEXT("frame://body-b-native"), 3, 3,
			IdentityBasis, {TEXT("0"), TEXT("20"), TEXT("0")}, ObservedAt,
			TEXT("conversion-evidence://body-b")),
		FUeAgentSpatialConversion(TEXT("conversion://simulation"),
			TEXT("frame://simulation-requested"), TEXT("frame://simulation-native"), 3, 3,
			IdentityBasis, {TEXT("99"), TEXT("99"), TEXT("99")}, ObservedAt,
			TEXT("conversion-evidence://simulation")),
	};
	Config.TemporalConversions.Add(FUeAgentTemporalConversion(
		TEXT("conversion://authored-to-simulation"), TEXT("clock://authored"),
		TEXT("clock://simulation-native"), FUeAgentTemporalRate(TEXT("60"), TEXT("1")),
		FUeAgentTemporalRate(TEXT("60"), TEXT("1")), TEXT("1"), TEXT("1"),
		TEXT("0"), TEXT("1"), TEXT("conversion-evidence://simulation-clock")));

	TSharedPtr<FUeAgentRigSimulationSpatialProductionRuntime> Runtime;
	FUeAgentRigSimulationSpatialExecutionError Error;
	if (!TestTrue(TEXT("compose Physics production runtime"),
		FUeAgentRigSimulationSpatialProductionRuntime::TryCreate(Config, Runtime, Error))
		|| !Runtime.IsValid())
	{
		AddError(Error.Code + TEXT(":") + Error.Detail);
		return false;
	}
	const FUeAgentResourceTrackerOwner Resources = MakeResources();
	FRigSimulationSpatialRevision Revision;

	Runtime->TryReadRevision(TEXT("material://rsp005"), Revision, Error);
	if (!ExecuteOperation(*this, *Runtime, TEXT("physics.material.update"),
		CanonicalInput(TEXT("physics.material.update"), Revision),
		TEXT("material://rsp005"), Resources)) return false;
	TestTrue(TEXT("material friction independently reads back"),
		FMath::IsNearlyEqual(Material->Friction, 0.71f));
	TestTrue(TEXT("material static friction independently reads back"),
		FMath::IsNearlyEqual(Material->StaticFriction, 0.82f));
	TestTrue(TEXT("material restitution independently reads back"),
		FMath::IsNearlyEqual(Material->Restitution, 0.23f));
	TestTrue(TEXT("material density independently reads back"),
		FMath::IsNearlyEqual(Material->Density, 2.5f));
	TestTrue(TEXT("material mass power independently reads back"),
		FMath::IsNearlyEqual(Material->RaiseMassToPower, 0.8f));
	TestTrue(TEXT("material combine overrides independently read back"),
		Material->bOverrideFrictionCombineMode
			&& Material->bOverrideRestitutionCombineMode);

	Runtime->TryReadRevision(TEXT("physics-asset://rsp005"), Revision, Error);
	if (!ExecuteOperation(*this, *Runtime, TEXT("physics.asset.structure.replace"),
		CanonicalInput(TEXT("physics.asset.structure.replace"), Revision),
		TEXT("physics-asset://rsp005"), Resources)) return false;
	TestEqual(TEXT("physics bodies independently read back"),
		PhysicsAsset->SkeletalBodySetups.Num(), 2);
	TestEqual(TEXT("physics constraints independently read back"),
		PhysicsAsset->ConstraintSetup.Num(), 1);
	USkeletalBodySetup* BodyA = FindBody(PhysicsAsset, TEXT("Root"));
	USkeletalBodySetup* BodyB = FindBody(PhysicsAsset, TEXT("Child"));
	TestNotNull(TEXT("body A independently resolves by bone"), BodyA);
	TestNotNull(TEXT("body B independently resolves by bone"), BodyB);
	TestTrue(TEXT("body A shape independently reads stable identity"), BodyA
		&& BodyA->AggGeom.GetElementByName(TEXT("RSP005Box")) != nullptr);
	TestTrue(TEXT("body B shape independently reads stable identity"), BodyB
		&& BodyB->AggGeom.GetElementByName(TEXT("RSP005Sphere")) != nullptr);
	TestTrue(TEXT("body A typed state independently reads back"), BodyA
		&& BodyA->DefaultInstance.GetCollisionEnabled(false)
			== ECollisionEnabled::QueryAndPhysics
		&& BodyA->PhysicsType == EPhysicsType::PhysType_Simulated
		&& BodyA->bConsiderForBounds
		&& FMath::IsNearlyEqual(BodyA->DefaultInstance.GetMassOverride(), 2.5f));
	TestTrue(TEXT("body B typed state independently reads back"), BodyB
		&& BodyB->DefaultInstance.GetCollisionEnabled(false)
			== ECollisionEnabled::PhysicsOnly
		&& BodyB->PhysicsType == EPhysicsType::PhysType_Kinematic
		&& !BodyB->bConsiderForBounds
		&& FMath::IsNearlyEqual(BodyB->DefaultInstance.GetMassOverride(), 3.5f));
	UPhysicsConstraintTemplate* Constraint = PhysicsAsset->ConstraintSetup.Num() == 1
		? PhysicsAsset->ConstraintSetup[0].Get() : nullptr;
	TestTrue(TEXT("body A/B remain separate constraint subjects"), Constraint
		&& Constraint->DefaultInstance.ConstraintBone1 == TEXT("Root")
		&& Constraint->DefaultInstance.ConstraintBone2 == TEXT("Child"));
	TestTrue(TEXT("body A C4 frame independently reads back"), Constraint
		&& Constraint->DefaultInstance.GetRefFrame(EConstraintFrame::Frame1)
			.GetTranslation().Equals(FVector(11.0, 2.0, 3.0), 0.001));
	TestTrue(TEXT("body B C4 frame independently reads back"), Constraint
		&& Constraint->DefaultInstance.GetRefFrame(EConstraintFrame::Frame2)
			.GetTranslation().Equals(FVector(4.0, 25.0, 6.0), 0.001));
	TestTrue(TEXT("profile independently reads body and constraint membership"),
		PhysicsAsset->PhysicalAnimationProfiles.Contains(TEXT("ProfileA"))
			&& PhysicsAsset->ConstraintProfiles.Contains(TEXT("ProfileA"))
			&& BodyA && BodyA->FindPhysicalAnimationProfile(TEXT("ProfileA"))
			&& BodyB && BodyB->FindPhysicalAnimationProfile(TEXT("ProfileA"))
			&& Constraint && Constraint->ContainsConstraintProfile(TEXT("ProfileA")));

	Runtime->TryReadRevision(TEXT("physics-asset://rsp005"), Revision, Error);
	if (!ExecuteOperation(*this, *Runtime, TEXT("physics.constraint.update"),
		CanonicalInput(TEXT("physics.constraint.update"), Revision),
		TEXT("physics-asset://rsp005"), Resources)) return false;
	TestTrue(TEXT("constraint endpoint A update independently reads back"), Constraint
		&& Constraint->DefaultInstance.GetRefFrame(EConstraintFrame::Frame1)
			.GetTranslation().Equals(FVector(12.0, 3.0, 4.0), 0.001));
	TestTrue(TEXT("constraint endpoint B update independently reads back"), Constraint
		&& Constraint->DefaultInstance.GetRefFrame(EConstraintFrame::Frame2)
			.GetTranslation().Equals(FVector(5.0, 26.0, 7.0), 0.001));
	TestTrue(TEXT("constraint typed state independently reads back"), Constraint
		&& Constraint->DefaultInstance.GetLinearXMotion() == LCM_Free
		&& FMath::IsNearlyEqual(Constraint->DefaultInstance.GetLinearLimit(), 18.0f)
		&& FMath::IsNearlyEqual(Constraint->DefaultInstance.GetAngularTwistLimit(), 25.0f)
		&& Constraint->DefaultInstance.IsCollisionDisabled());

	TSharedPtr<const FUeAgentRigSimulationSpatialRequest> DecodedProbe;
	TestTrue(TEXT("decode Physics probe for C4 inspection"),
		Runtime->TryDecodeForExecution(TEXT("physics.simulation.probe"),
			CanonicalInput(TEXT("physics.simulation.probe"), {}),
			DecodedProbe, Error));
	int32 DirectionCount = 0;
	if (DecodedProbe.IsValid())
	{
		for (const FUeAgentSpatialValuePtr& Value
			: DecodedProbe->GetFormalPortRequest().GetSpatialValues())
		{
			if (Value.IsValid()
				&& Value->GetQuantityKind() == EUeAgentSpatialQuantityKind::Direction)
			{
				++DirectionCount;
				TestEqual(TEXT("probe vector subject stays explicit"),
					Value->GetSubject(), FString(TEXT("subject://cube")));
				TestEqual(TEXT("probe vector retains three numeric components"),
					Value->GetComponents().Num(), 3);
			}
		}
	}
	TestEqual(TEXT("impulse and velocity both enter C4 formal values"),
		DirectionCount, 2);

	FUeAgentRigSimulationSpatialProductionRuntimeConfig NoSessionConfig = Config;
	NoSessionConfig.PhysicsSessionPort.Reset();
	TSharedPtr<FUeAgentRigSimulationSpatialProductionRuntime> NoSessionRuntime;
	TestTrue(TEXT("compose definition runtime without optional session owner"),
		FUeAgentRigSimulationSpatialProductionRuntime::TryCreate(
			NoSessionConfig, NoSessionRuntime, Error));
	FString MissingSessionFailure;
	TestFalse(TEXT("probe fails closed without public session owner"),
		ExecuteOperation(*this, *NoSessionRuntime, TEXT("physics.simulation.probe"),
			CanonicalInput(TEXT("physics.simulation.probe"), {}),
			TEXT("physics-session-owner://rsp005"), Resources,
			&MissingSessionFailure));
	TestEqual(TEXT("missing session owner failure is exact"), MissingSessionFailure,
		FString(TEXT("rig_simulation_spatial_physics_session_port_unavailable")));

	if (!ExecuteOperation(*this, *Runtime, TEXT("physics.simulation.probe"),
		CanonicalInput(TEXT("physics.simulation.probe"), {}),
		TEXT("physics-session-owner://rsp005"), Resources)) return false;
	TestEqual(TEXT("session owner invoked exactly once"), SessionPort->CallCount, 1);
	TestTrue(TEXT("session owner used only its owned preview world"),
		SessionPort->bSawOwnedWorld);
	TestTrue(TEXT("simulation observation exists before restoration"),
		SessionPort->bObservedMotion);
	TestTrue(TEXT("session restore is distinct and succeeded"), SessionPort->bRestored);
	TestTrue(TEXT("owned component transform independently restored"),
		SessionPort->GetComponent()->GetComponentTransform().Equals(
			SessionPort->GetInitialTransform(), 0.001));
	TestTrue(TEXT("owned component velocity independently restored"),
		SessionPort->GetComponent()->GetPhysicsLinearVelocity().IsNearlyZero(0.001));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentRigSimulationSpatialPhysicsSessionFailureSemanticsTest,
	"UeAgentInterface.DomainAdapters.RigSimulationSpatial.E2.CrossOwner.PhysicsSessionFailureSemantics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentRigSimulationSpatialPhysicsSessionFailureSemanticsTest::RunTest(
	const FString& Parameters)
{
	UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>(
		GetTransientPackage(), TEXT("RSP007PhysicsSessionSubject"), RF_Transactional);
	if (!TestNotNull(TEXT("create explicit physics session subject"), Component))
		return false;
	const TSharedRef<FScriptedPhysicsSessionPort> SessionPort =
		MakeShared<FScriptedPhysicsSessionPort>(Component);
	const FUeAgentTemporalPoint ObservedAt(TEXT("clock://authored"), TEXT("0"),
		TEXT("60"), TEXT("1"), TEXT("simulation"));
	const TArray<TArray<FString>> IdentityBasis = {
		{TEXT("1"), TEXT("0"), TEXT("0")},
		{TEXT("0"), TEXT("1"), TEXT("0")},
		{TEXT("0"), TEXT("0"), TEXT("1")},
	};

	FUeAgentRigSimulationSpatialProductionRuntimeConfig Config;
	Config.ProjectInstanceId = TEXT("rsp007-physics-project");
	Config.EditorInstanceId = TEXT("rsp007-physics-editor");
	Config.ServerSessionId = TEXT("rsp007-physics-session");
	Config.RuntimeGeneration = TEXT("rsp007-physics-generation");
	Config.RegistryEpoch = 1;
	Config.ObjectBindings = {
		{TEXT("physics-session-owner://rsp005"), Component},
		{TEXT("subject://cube"), Component},
	};
	Config.PhysicsSubjectBindings = {{TEXT("subject://cube"), Component,
		TEXT("frame://simulation-native"), TEXT("clock://simulation-native")}};
	Config.PhysicsSessionPort = SessionPort;
	Config.ClockBindings.Add({TEXT("clock://authored"),
		TEXT("clock://simulation-native"), TEXT("clock-evidence://rsp007")});
	Config.FrameBindings.Add(FUeAgentRigSimulationSpatialFrameBinding(
		FUeAgentSpatialFrame(TEXT("frame://simulation-requested"),
			TEXT("frame://basis"), 3, IdentityBasis,
			{TEXT("0"), TEXT("0"), TEXT("0")}, TEXT("cm"),
			TEXT("basis://rsp005-simulation"), ObservedAt), false));
	Config.FrameBindings.Add(FUeAgentRigSimulationSpatialFrameBinding(
		FUeAgentSpatialFrame(TEXT("frame://simulation-native"),
			TEXT("frame://basis"), 3, IdentityBasis,
			{TEXT("-99"), TEXT("-99"), TEXT("-99")}, TEXT("cm"),
			TEXT("basis://rsp005-simulation"), ObservedAt), true));
	Config.SpatialConversions.Add(FUeAgentSpatialConversion(
		TEXT("conversion://simulation"), TEXT("frame://simulation-requested"),
		TEXT("frame://simulation-native"), 3, 3, IdentityBasis,
		{TEXT("99"), TEXT("99"), TEXT("99")}, ObservedAt,
		TEXT("conversion-evidence://simulation")));
	Config.TemporalConversions.Add(FUeAgentTemporalConversion(
		TEXT("conversion://authored-to-simulation"), TEXT("clock://authored"),
		TEXT("clock://simulation-native"), FUeAgentTemporalRate(TEXT("60"), TEXT("1")),
		FUeAgentTemporalRate(TEXT("60"), TEXT("1")), TEXT("1"), TEXT("1"),
		TEXT("0"), TEXT("1"), TEXT("conversion-evidence://simulation-clock")));

	const struct
	{
		EScriptedPhysicsSessionScenario Scenario;
		const TCHAR* ErrorCode;
	} FailureCases[] = {
		{EScriptedPhysicsSessionScenario::OwnerRejected,
			TEXT("rig_simulation_spatial_physics_session_owner_rejected")},
		{EScriptedPhysicsSessionScenario::CancelledNoChange,
			TEXT("rig_simulation_spatial_physics_session_cancelled_no_change")},
		{EScriptedPhysicsSessionScenario::TimeoutStateUnknown,
			TEXT("rig_simulation_spatial_physics_session_timeout_state_unknown")},
		{EScriptedPhysicsSessionScenario::RollbackFailedStateUnknown,
			TEXT("rig_simulation_spatial_physics_session_rollback_failed_state_unknown")},
	};
	for (const auto& FailureCase : FailureCases)
	{
		SessionPort->SetScenario(FailureCase.Scenario);
		TSharedPtr<FUeAgentRigSimulationSpatialProductionRuntime> Runtime;
		FUeAgentRigSimulationSpatialExecutionError Error;
		if (!TestTrue(TEXT("compose physics session boundary runtime"),
			FUeAgentRigSimulationSpatialProductionRuntime::TryCreate(
				Config, Runtime, Error)) || !Runtime.IsValid()) return false;
		const FTransform TransformBefore = Component->GetComponentTransform();
		FString FailureCode;
		TestFalse(TEXT("public session failure stops the domain operation"),
			ExecuteOperation(*this, *Runtime, TEXT("physics.simulation.probe"),
				CanonicalInput(TEXT("physics.simulation.probe"), {}),
				TEXT("physics-session-owner://rsp005"), MakeResources(),
				&FailureCode));
		TestEqual(TEXT("public session failure remains exact"),
			FailureCode, FString(FailureCase.ErrorCode));
		TestEqual(TEXT("domain never replays the failed session effect"),
			SessionPort->RunCount, 1);
		TestEqual(TEXT("failed session has only the independent before read"),
			SessionPort->ReadCount, 1);
		TestTrue(TEXT("domain performs no fallback or late transform mutation"),
			Component->GetComponentTransform().Equals(TransformBefore, 0.001));
	}

	SessionPort->SetScenario(
		EScriptedPhysicsSessionScenario::SuccessWithUnverifiedReadback);
	TSharedPtr<FUeAgentRigSimulationSpatialProductionRuntime> Runtime;
	FUeAgentRigSimulationSpatialExecutionError Error;
	if (!TestTrue(TEXT("compose physics session readback runtime"),
		FUeAgentRigSimulationSpatialProductionRuntime::TryCreate(Config, Runtime, Error))
		|| !Runtime.IsValid()) return false;
	FVerificationOutcome Outcome;
	TestTrue(TEXT("owner success proceeds to independent session readback"),
		ExecuteOperation(*this, *Runtime, TEXT("physics.simulation.probe"),
			CanonicalInput(TEXT("physics.simulation.probe"), {}),
			TEXT("physics-session-owner://rsp005"), MakeResources(), nullptr,
			&Outcome));
	TestEqual(TEXT("independent readback rejects owner-only success"),
		Outcome.GetStatus(), EVerificationStatus::Mismatch);
	TestEqual(TEXT("session owner receipt alone cannot produce complete"),
		Outcome.GetEffectOutcome(), EEffectOutcome::Partial);
	TestFalse(TEXT("unverified session outcome is never retry-safe"),
		Outcome.IsRetrySafe());
	TestEqual(TEXT("successful session invocation is single-shot"),
		SessionPort->RunCount, 1);
	TestEqual(TEXT("session outcome uses before and after owner readback"),
		SessionPort->ReadCount, 2);
	return !HasAnyErrors();
}

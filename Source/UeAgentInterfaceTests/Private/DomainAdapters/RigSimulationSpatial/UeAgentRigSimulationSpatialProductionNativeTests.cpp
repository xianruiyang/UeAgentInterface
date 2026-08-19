#include "Misc/AutomationTest.h"

#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Animation/SkeletalMeshActor.h"
#include "Components/SkeletalMeshComponent.h"
#include "Core/BindingGateway/Ticket/UeAgentBindingTicket.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialNative.h"
#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialProductionRuntime.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "PreviewScene.h"
#include "Rigs/FKControlRig.h"
#include "Sequencer/MovieSceneControlRigParameterSection.h"
#include "Sections/MovieSceneSkeletalAnimationSection.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Sequencer/MovieSceneControlRigParameterTrack.h"
#include "Tracks/MovieSceneSkeletalAnimationTrack.h"
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

	struct FFixture
	{
		TUniquePtr<FPreviewScene> PreviewScene;
		USkeleton* Skeleton = nullptr;
		USkeletalMesh* Mesh = nullptr;
		UAnimSequence* SourceAnimation = nullptr;
		UAnimSequence* TargetAnimation = nullptr;
		ULevelSequence* Sequence = nullptr;
		ASkeletalMeshActor* Actor = nullptr;
		FGuid BindingGuid;
		FName RootBone;
	};

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

	TSharedPtr<FJsonObject> RevisionJson(const FRigSimulationSpatialRevision& Revision)
	{
		const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("source_id"), Revision.SourceId);
		Result->SetStringField(TEXT("generation"), Revision.Generation);
		Result->SetStringField(TEXT("sequence"), LexToString(Revision.Sequence));
		return Result;
	}

	TSharedPtr<FJsonObject> RateJson()
	{
		const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("numerator"), TEXT("30"));
		Result->SetStringField(TEXT("denominator"), TEXT("1"));
		return Result;
	}

	TSharedPtr<FJsonObject> RangeJson()
	{
		const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("clock_id"), TEXT("clock://authored"));
		Result->SetStringField(TEXT("lower_tick"), TEXT("0"));
		Result->SetStringField(TEXT("upper_tick"), TEXT("2"));
		Result->SetObjectField(TEXT("rate"), RateJson());
		Result->SetStringField(TEXT("state"), TEXT("authored"));
		return Result;
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

	TSharedPtr<FJsonObject> SocketTransformJson()
	{
		const TSharedPtr<FJsonObject> Frame = MakeShared<FJsonObject>();
		Frame->SetStringField(TEXT("frame_id"), TEXT("frame://requested"));
		Frame->SetStringField(TEXT("basis_frame_id"), TEXT("frame://basis"));
		Frame->SetNumberField(TEXT("dimension"), 3);
		Frame->SetStringField(TEXT("unit"), TEXT("cm"));
		Frame->SetStringField(TEXT("basis_evidence_id"), TEXT("basis://rsp003"));
		const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("subject"), TEXT("socket://rsp003"));
		Result->SetStringField(TEXT("quantity"), TEXT("transform"));
		Result->SetObjectField(TEXT("frame"), Frame);
		Result->SetArrayField(TEXT("components"), StringValues({
			TEXT("1"), TEXT("0"), TEXT("0"), TEXT("2"),
			TEXT("0"), TEXT("1"), TEXT("0"), TEXT("0"),
			TEXT("0"), TEXT("0"), TEXT("1"), TEXT("0"),
			TEXT("0"), TEXT("0"), TEXT("0"), TEXT("1")}));
		Result->SetArrayField(TEXT("conversion_evidence_ids"),
			StringValues({TEXT("conversion://request-evidence")}));
		return Result;
	}

	FUeAgentContractValue CanonicalInput(
		const FString& Capability,
		const FRigSimulationSpatialRevision& Revision)
	{
		const TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetObjectField(TEXT("expected_revision"), RevisionJson(Revision));
		if (Capability == TEXT("rig.skeleton.preview_mesh.bind"))
		{
			Root->SetStringField(TEXT("skeleton_ref"), TEXT("skeleton://rsp003"));
			Root->SetStringField(TEXT("preview_mesh_ref"), TEXT("mesh://rsp003"));
		}
		else if (Capability == TEXT("rig.skeleton.socket.update"))
		{
			Root->SetStringField(TEXT("skeleton_ref"), TEXT("skeleton://rsp003"));
			Root->SetStringField(TEXT("operation"), TEXT("create"));
			Root->SetStringField(TEXT("socket_ref"), TEXT("socket://rsp003"));
			Root->SetStringField(TEXT("parent_bone_ref"), TEXT("bone://root"));
			Root->SetObjectField(TEXT("socket_transform"), SocketTransformJson());
		}
		else if (Capability == TEXT("rig.control.bake.animation"))
		{
			Root->SetStringField(TEXT("source_sequence_ref"), TEXT("sequence://rsp003"));
			Root->SetStringField(TEXT("target_animation_ref"), TEXT("animation://target"));
			Root->SetStringField(TEXT("binding_ref"), TEXT("binding://rsp003"));
			Root->SetStringField(TEXT("rig_ref"), TEXT("rig://fk-class"));
			Root->SetObjectField(TEXT("authored_range"), RangeJson());
			Root->SetObjectField(TEXT("sample_rate"), RateJson());
		}
		else if (Capability == TEXT("rig.control.bake.controls"))
		{
			Root->SetStringField(TEXT("target_sequence_ref"), TEXT("sequence://rsp003"));
			Root->SetStringField(TEXT("binding_ref"), TEXT("binding://rsp003"));
			Root->SetStringField(TEXT("rig_ref"), TEXT("rig://fk-class"));
			Root->SetObjectField(TEXT("authored_range"), RangeJson());
			Root->SetObjectField(TEXT("sample_rate"), RateJson());
		}
		FString Text;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
		return FUeAgentContractValue(MoveTemp(Text));
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
		{
			return nullptr;
		}
		FUeAgentExecutionContextInput Input;
		Input.ProjectInstanceId = TEXT("rsp003-project");
		Input.EditorInstanceId = TEXT("rsp003-editor");
		Input.StatePlane = Planes[0];
		Input.WorldType = EUeAgentExecutionWorldType::Preview;
		Input.WorldIdentity = TEXT("world://rsp003-owned-preview");
		Input.AssetEditorIdentity = TEXT("asset-editor://none");
		Input.ViewportIdentity = TEXT("viewport://none");
		Input.ContextRevision = TEXT("rsp003-context-1");
		Input.bResolvedFromCurrent = false;
		FUeAgentExecutionContextRefPtr Execution;
		FUeAgentExecutionContextError ExecutionError;
		if (!Test.TestTrue(TEXT("freeze explicit rig/spatial context"),
			FUeAgentExecutionContextRef::TryCreate(Input, DescriptorValue.GetIdentity(),
				EUeAgentRequestKind::Operation,
				DescriptorValue.GetExecutionContextPolicy(), Execution, ExecutionError)))
		{
			return nullptr;
		}
		const FUeAgentCallerContext Caller(TEXT("rsp003-native-e2"),
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
			FUeAgentOperationContext::TryCreate(Request, Caller,
				Execution.ToSharedRef(), Resources, Deadline, Cancellation.GetToken(),
				Result, Error));
		return Result;
	}

	TArray<FUeAgentOperationTargetBinding> MakeTargets(
		FAutomationTestBase& Test,
		const FUeAgentOperationDescriptor& DescriptorValue,
		const FString& Capability,
		const FString& PublicRef)
	{
		const FUeAgentCandidateBinding Candidate(
			TEXT("rsp003-binding:") + Capability,
			FUeAgentCandidateSummary(
				FUeAgentCanonicalIdentity(TEXT("rsp003-target:") + Capability,
					TEXT("rig_simulation_spatial_target"), TEXT("rsp003-owner"),
					PublicRef, TEXT("target"), TEXT("rsp003-generation")),
				DescriptorValue.GetIdentity(), TEXT("rig_simulation_spatial_target"),
				PublicRef, TEXT("owned RSP-003 fixture"),
				{TEXT("RigSimulationSpatial"), TEXT("Fixture")}),
			FUeAgentCatalogRevision(TEXT("rsp003-catalog"),
				TEXT("rsp003-generation"), 1), PublicRef,
			TEXT("rig-simulation-spatial-native"),
			{EUeAgentCandidateOperation::Read, EUeAgentCandidateOperation::Update},
			TEXT("rsp003-native-binding"));
		TSharedPtr<const FUeAgentOperationTicketService> Service;
		FUeAgentOperationTicketError Error;
		if (!Test.TestTrue(TEXT("create operation ticket service"),
			FUeAgentOperationTicketService::TryCreate(TEXT("rsp003-integrity"), 60.0,
				[](){ return 100.0; }, [](){ return TEXT("rsp003-ticket"); },
				Service, Error)))
		{
			return {};
		}
		const FUeAgentOperationSessionIdentity Session(TEXT("rsp003-session"), 1,
			TEXT("sha256:0000000000000000000000000000000000000000000000000000000000000000"));
		const FUeAgentCallerContext Caller(TEXT("rsp003-native-e2"),
			{TEXT("rig-simulation-spatial:read"),
				TEXT("rig-simulation-spatial:write")}, TEXT("corr-ticket-") + Capability);
		TOptional<FUeAgentOperationTicket> Ticket;
		if (!Test.TestTrue(TEXT("issue explicit target ticket"),
			Service->TryIssue(Candidate, DescriptorValue.GetIdentity(), Session,
				Caller, {}, Ticket, Error)) || !Ticket.IsSet())
		{
			return {};
		}
		return {FUeAgentOperationTargetBinding(Ticket.GetValue(), Candidate)};
	}

	bool ExecuteOperation(
		FAutomationTestBase& Test,
		const FUeAgentRigSimulationSpatialProductionRuntime& Runtime,
		const FString& Capability,
		const FUeAgentContractValue& Input,
		const FString& PublicRef,
		const FUeAgentResourceTrackerOwner& Resources)
	{
		const auto* ContractValue = Contract(Capability);
		const auto* DescriptorValue = Descriptor(Runtime, Capability);
		if (!Test.TestNotNull(TEXT("operation contract"), ContractValue)
			|| !Test.TestNotNull(TEXT("operation descriptor"), DescriptorValue))
		{
			return false;
		}
		TSharedPtr<const FUeAgentRigSimulationSpatialRequest> Decoded;
		FUeAgentRigSimulationSpatialExecutionError DecodeError;
		if (!Test.TestTrue(TEXT("production decode before C3O: ") + Capability,
			Runtime.TryDecodeForExecution(Capability, Input, Decoded, DecodeError)))
		{
			Test.AddError(Capability + TEXT(":decode:") + DecodeError.Code
				+ TEXT(":") + DecodeError.Detail);
			return false;
		}
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
		{
			return false;
		}
		FUeAgentOperationBindingError Error;
		TOptional<FPlanningRequest> PlanningRequest;
		if (!Test.TestTrue(TEXT("C3O decode and preflight: ") + Capability,
			(*Chain)->TryBuildPlanningRequest(Input, Targets, *Context,
				PlanningRequest, Error)) || !PlanningRequest.IsSet())
		{
			Test.AddError(Capability + TEXT(":preflight:") + Error.Code
				+ TEXT(":") + Error.Detail);
			return false;
		}
		FPlanningResult Planning;
		if (!Test.TestTrue(TEXT("C3O plan: ") + Capability,
			(*Chain)->GetPlanner().TryPlan(*PlanningRequest, *Context, Planning, Error)))
		{
			Test.AddError(Capability + TEXT(":plan:") + Error.Code
				+ TEXT(":") + Error.Detail);
			return false;
		}
		FExecutionReceipt Receipt;
		if (!Test.TestTrue(TEXT("C3O mutate once: ") + Capability,
			(*Chain)->GetExecutor().TryExecute(Planning, Targets, *Context, Receipt, Error)))
		{
			Test.AddError(Capability + TEXT(":execute:") + Error.Code
				+ TEXT(":") + Error.Detail);
			return false;
		}
		FVerificationOutcome Outcome;
		if (!Test.TestTrue(TEXT("C3O independent readback: ") + Capability,
			(*Chain)->GetVerifier().TryVerify(Receipt, Targets, *Context, Outcome, Error)))
		{
			Test.AddError(Capability + TEXT(":verify:") + Error.Code
				+ TEXT(":") + Error.Detail);
			return false;
		}
		return Test.TestEqual(TEXT("exact production C3O verified: ") + Capability,
			Outcome.GetStatus(), EVerificationStatus::Verified);
	}

	bool CreateFixture(FAutomationTestBase& Test, FFixture& Out)
	{
		USkeletalMesh* SourceMesh = LoadObject<USkeletalMesh>(nullptr,
			TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
		if (!Test.TestNotNull(TEXT("load real skeletal mesh"), SourceMesh)
			|| !Test.TestNotNull(TEXT("real source skeleton"),
				SourceMesh ? SourceMesh->GetSkeleton() : nullptr))
		{
			return false;
		}
		UPackage* RigPackage = CreatePackage(TEXT("/Game/__UAI_RSP003/RigAssets"));
		Out.Skeleton = DuplicateObject<USkeleton>(
			SourceMesh->GetSkeleton(), RigPackage, TEXT("Skeleton"));
		Out.Mesh = DuplicateObject<USkeletalMesh>(SourceMesh, RigPackage, TEXT("Mesh"));
		if (!Test.TestNotNull(TEXT("duplicate owned skeleton"), Out.Skeleton)
			|| !Test.TestNotNull(TEXT("duplicate owned mesh"), Out.Mesh)) return false;
		Out.Mesh->SetSkeleton(Out.Skeleton);
		Out.Skeleton->SetPreviewMesh(nullptr, false);
		Out.RootBone = Out.Skeleton->GetReferenceSkeleton().GetBoneName(0);

		UPackage* AnimationPackage = CreatePackage(
			TEXT("/Game/__UAI_RSP003/Animations"));
		Out.SourceAnimation = NewObject<UAnimSequence>(AnimationPackage,
			TEXT("SourceAnimation"), RF_Public | RF_Standalone | RF_Transactional);
		Out.TargetAnimation = NewObject<UAnimSequence>(AnimationPackage,
			TEXT("TargetAnimation"), RF_Public | RF_Standalone | RF_Transactional);
		for (UAnimSequence* Animation : {Out.SourceAnimation, Out.TargetAnimation})
		{
			Animation->SetSkeleton(Out.Skeleton);
			Animation->GetController().InitializeModel();
			Animation->GetController().SetFrameRate(FFrameRate(30, 1), false);
		}
		IAnimationDataController& Controller = Out.SourceAnimation->GetController();
		Controller.SetNumberOfFrames(FFrameNumber(30), false);
		Controller.AddBoneCurve(Out.RootBone, false);
		TArray<FVector3f> Positions;
		TArray<FQuat4f> Rotations;
		TArray<FVector3f> Scales;
		for (int32 Index = 0; Index <= 30; ++Index)
		{
			Positions.Add(FVector3f(static_cast<float>(Index), 0.0f, 0.0f));
			Rotations.Add(FQuat4f::Identity);
			Scales.Add(FVector3f::OneVector);
		}
		Controller.SetBoneTrackKeys(Out.RootBone, Positions, Rotations, Scales, false);
		Controller.NotifyPopulated();

		Out.PreviewScene = MakeUnique<FPreviewScene>(FPreviewScene::ConstructionValues()
			.SetCreateDefaultLighting(false).SetCreatePhysicsScene(false)
			.SetForceMipsResident(false).ShouldSimulatePhysics(false)
			.SetTransactional(false));
		UWorld* World = Out.PreviewScene->GetWorld();
		if (!Test.TestNotNull(TEXT("create owned preview world"), World)) return false;
		Out.Actor = World->SpawnActor<ASkeletalMeshActor>();
		if (!Test.TestNotNull(TEXT("spawn explicitly owned skeletal actor"), Out.Actor))
		{
			return false;
		}
		Out.Actor->GetSkeletalMeshComponent()->SetSkeletalMeshAsset(Out.Mesh);

		UPackage* SequencePackage = CreatePackage(TEXT("/Game/__UAI_RSP003/Sequence"));
		Out.Sequence = NewObject<ULevelSequence>(SequencePackage, TEXT("Sequence"),
			RF_Public | RF_Standalone | RF_Transactional);
		Out.Sequence->Initialize();
		UMovieScene* MovieScene = Out.Sequence->GetMovieScene();
		MovieScene->SetDisplayRate(FFrameRate(30, 1));
		MovieScene->SetTickResolutionDirectly(FFrameRate(30, 1));
		MovieScene->SetPlaybackRange(0, 30);
		Out.BindingGuid = MovieScene->AddPossessable(
			TEXT("RSP003Actor"), Out.Actor->GetClass());
		Out.Sequence->BindPossessableObject(Out.BindingGuid, *Out.Actor, World);
		UMovieSceneSkeletalAnimationTrack* Track =
			MovieScene->AddTrack<UMovieSceneSkeletalAnimationTrack>(Out.BindingGuid);
		if (!Test.TestNotNull(TEXT("create skeletal animation track"), Track)) return false;
		UMovieSceneSkeletalAnimationSection* Section =
			Cast<UMovieSceneSkeletalAnimationSection>(Track->CreateNewSection());
		if (!Test.TestNotNull(TEXT("create skeletal animation section"), Section)) return false;
		Section->Params.Animation = Out.SourceAnimation;
		Section->SetRange(TRange<FFrameNumber>(FFrameNumber(0), FFrameNumber(30)));
		Track->AddSection(*Section);
		return true;
	}

	int32 CountControlKeys(const ULevelSequence& Sequence)
	{
		int32 Result = 0;
		const UMovieScene* Scene = Sequence.GetMovieScene();
		if (!Scene) return Result;
		for (const FMovieSceneBinding& Binding : Scene->GetBindings())
		{
			for (const UMovieSceneTrack* Track : Binding.GetTracks())
			{
				const UMovieSceneControlRigParameterTrack* RigTrack =
					Cast<UMovieSceneControlRigParameterTrack>(Track);
				if (!RigTrack) continue;
				for (const UMovieSceneSection* RawSection : RigTrack->GetAllSections())
				{
					const UMovieSceneControlRigParameterSection* Section =
						Cast<UMovieSceneControlRigParameterSection>(RawSection);
					if (!Section) continue;
					for (const FTransformParameterNameAndCurves& Curves
						: Section->GetTransformParameterNamesAndCurves())
					{
						for (int32 Axis = 0; Axis < 3; ++Axis)
						{
							Result += Curves.Translation[Axis].GetData().GetTimes().Num();
							Result += Curves.Rotation[Axis].GetData().GetTimes().Num();
							Result += Curves.Scale[Axis].GetData().GetTimes().Num();
						}
					}
				}
			}
		}
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentRigSimulationSpatialProductionNativeTest,
	"UeAgentInterface.DomainAdapters.RigSimulationSpatial.E2.ProductionNative.SkeletalAttachmentAndRigBake",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentRigSimulationSpatialProductionNativeTest::RunTest(
	const FString& Parameters)
{
	FFixture Fixture;
	if (!CreateFixture(*this, Fixture)) return false;
	const FUeAgentTemporalPoint ObservedAt(TEXT("clock://socket"), TEXT("0"),
		TEXT("30"), TEXT("1"), TEXT("authored"));
	const TArray<TArray<FString>> IdentityBasis = {
		{TEXT("1"), TEXT("0"), TEXT("0")},
		{TEXT("0"), TEXT("1"), TEXT("0")},
		{TEXT("0"), TEXT("0"), TEXT("1")},
	};

	FUeAgentRigSimulationSpatialProductionRuntimeConfig Config;
	Config.ProjectInstanceId = TEXT("rsp003-project");
	Config.EditorInstanceId = TEXT("rsp003-editor");
	Config.ServerSessionId = TEXT("rsp003-session");
	Config.RuntimeGeneration = TEXT("rsp003-generation");
	Config.RegistryEpoch = 1;
	Config.ObjectBindings = {
		{TEXT("skeleton://rsp003"), Fixture.Skeleton},
		{TEXT("mesh://rsp003"), Fixture.Mesh},
		{TEXT("sequence://rsp003"), Fixture.Sequence},
		{TEXT("animation://target"), Fixture.TargetAnimation},
		{TEXT("rig://fk-class"), UFKControlRig::StaticClass()},
		{TEXT("ik://unsupported"), GetTransientPackage()},
	};
	FUeAgentRigSimulationSpatialSkeletonBinding SkeletonBinding;
	SkeletonBinding.Identity = TEXT("skeleton://rsp003");
	SkeletonBinding.Skeleton = Fixture.Skeleton;
	Config.SkeletonBindings.Add(SkeletonBinding);
	FUeAgentRigSimulationSpatialSocketBinding SocketBinding;
	SocketBinding.SkeletonIdentity = TEXT("skeleton://rsp003");
	SocketBinding.SocketIdentity = TEXT("socket://rsp003");
	SocketBinding.SocketName = TEXT("RSP003Socket");
	SocketBinding.ParentBoneIdentity = TEXT("bone://root");
	SocketBinding.ParentBoneName = Fixture.RootBone;
	SocketBinding.NativeParentFrameId = TEXT("frame://parent-bone");
	Config.SocketBindings.Add(SocketBinding);
	FUeAgentRigSimulationSpatialRigBakeBinding RigBakeBinding;
	RigBakeBinding.Identity = TEXT("binding://rsp003");
	RigBakeBinding.World = Fixture.PreviewScene->GetWorld();
	RigBakeBinding.Sequence = Fixture.Sequence;
	RigBakeBinding.SkeletalMeshComponent = Fixture.Actor->GetSkeletalMeshComponent();
	RigBakeBinding.BindingGuid = Fixture.BindingGuid;
	RigBakeBinding.NativeAuthoredClockId = TEXT("clock://sequence-native");
	RigBakeBinding.NativeControlFrameId = TEXT("frame://control-native");
	Config.RigBakeBindings.Add(RigBakeBinding);
	Config.ClockBindings.Add({TEXT("clock://authored"),
		TEXT("clock://sequence-native"), TEXT("clock-evidence://rsp003")});
	Config.FrameBindings.Add(FUeAgentRigSimulationSpatialFrameBinding(
		FUeAgentSpatialFrame(TEXT("frame://requested"), TEXT("frame://basis"), 3,
			IdentityBasis, {TEXT("10"), TEXT("0"), TEXT("0")}, TEXT("cm"),
			TEXT("basis://rsp003"), ObservedAt), false));
	Config.FrameBindings.Add(FUeAgentRigSimulationSpatialFrameBinding(
		FUeAgentSpatialFrame(TEXT("frame://parent-bone"), TEXT("frame://basis"), 3,
			IdentityBasis, {TEXT("0"), TEXT("0"), TEXT("0")}, TEXT("cm"),
			TEXT("basis://rsp003"), ObservedAt), true));
	Config.FrameBindings.Add(FUeAgentRigSimulationSpatialFrameBinding(
		FUeAgentSpatialFrame(TEXT("frame://control-native"), TEXT("frame://basis"), 3,
			IdentityBasis, {TEXT("0"), TEXT("0"), TEXT("0")}, TEXT("cm"),
			TEXT("basis://rsp003"), ObservedAt), true));
	Config.SpatialConversions.Add(FUeAgentSpatialConversion(
		TEXT("conversion://requested-to-parent"), TEXT("frame://requested"),
		TEXT("frame://parent-bone"), 3, 3, IdentityBasis,
		{TEXT("10"), TEXT("0"), TEXT("0")}, ObservedAt,
		TEXT("conversion-evidence://requested-to-parent")));
	Config.TemporalConversions.Add(FUeAgentTemporalConversion(
		TEXT("conversion://authored-to-sequence"), TEXT("clock://authored"),
		TEXT("clock://sequence-native"), FUeAgentTemporalRate(TEXT("30"), TEXT("1")),
		FUeAgentTemporalRate(TEXT("30"), TEXT("1")), TEXT("1"), TEXT("1"),
		TEXT("0"), TEXT("1"), TEXT("conversion-evidence://authored-to-sequence")));

	TSharedPtr<FUeAgentRigSimulationSpatialProductionRuntime> Runtime;
	FUeAgentRigSimulationSpatialExecutionError Error;
	if (!TestTrue(TEXT("compose real explicitly bound production runtime"),
		FUeAgentRigSimulationSpatialProductionRuntime::TryCreate(Config, Runtime, Error))
		|| !Runtime.IsValid())
	{
		AddError(Error.Code + TEXT(":") + Error.Detail);
		return false;
	}
	const FUeAgentResourceTrackerOwner Resources = MakeResources();
	FRigSimulationSpatialRevision Revision;

	Runtime->TryReadRevision(TEXT("skeleton://rsp003"), Revision, Error);
	ExecuteOperation(*this, *Runtime, TEXT("rig.skeleton.preview_mesh.bind"),
		CanonicalInput(TEXT("rig.skeleton.preview_mesh.bind"), Revision),
		TEXT("skeleton://rsp003"), Resources);
	TestEqual(TEXT("preview mesh independently reads back from real skeleton"),
		Fixture.Skeleton->GetPreviewMesh(true), Fixture.Mesh);

	Runtime->TryReadRevision(TEXT("skeleton://rsp003"), Revision, Error);
	ExecuteOperation(*this, *Runtime, TEXT("rig.skeleton.socket.update"),
		CanonicalInput(TEXT("rig.skeleton.socket.update"), Revision),
		TEXT("skeleton://rsp003"), Resources);
	const USkeletalMeshSocket* Socket = Fixture.Skeleton->FindSocket(TEXT("RSP003Socket"));
	TestNotNull(TEXT("socket independently reads back from real skeleton"), Socket);
	if (Socket)
	{
		TestEqual(TEXT("socket parent independently reads back"),
			Socket->BoneName, Fixture.RootBone);
		TestTrue(TEXT("socket C4 transform is written in explicit parent frame"),
			Socket->RelativeLocation.Equals(FVector(12.0, 0.0, 0.0), 0.001));
	}

	Runtime->TryReadRevision(TEXT("animation://target"), Revision, Error);
	ExecuteOperation(*this, *Runtime, TEXT("rig.control.bake.animation"),
		CanonicalInput(TEXT("rig.control.bake.animation"), Revision),
		TEXT("animation://target"), Resources);
	const IAnimationDataModel* DataModel = Fixture.TargetAnimation->GetDataModel();
	TArray<FName> BoneTrackNames;
	if (DataModel) DataModel->GetBoneTrackNames(BoneTrackNames);
	TestTrue(TEXT("baked animation independently exposes bone tracks"),
		DataModel && BoneTrackNames.Contains(Fixture.RootBone)
			&& DataModel->GetNumberOfKeys() > 0);

	Runtime->TryReadRevision(TEXT("sequence://rsp003"), Revision, Error);
	ExecuteOperation(*this, *Runtime, TEXT("rig.control.bake.controls"),
		CanonicalInput(TEXT("rig.control.bake.controls"), Revision),
		TEXT("sequence://rsp003"), Resources);
	TestTrue(TEXT("baked controls independently expose authored control keys"),
		CountControlKeys(*Fixture.Sequence) > 0);

	const auto* UnsupportedDescriptor = Descriptor(*Runtime,
		TEXT("rig.ik.definition.update"));
	const auto* UnsupportedContract = Contract(TEXT("rig.ik.definition.update"));
	if (TestNotNull(TEXT("unsupported descriptor exists"), UnsupportedDescriptor)
		&& TestNotNull(TEXT("unsupported contract exists"), UnsupportedContract))
	{
		const auto Context = MakeContext(*this, *UnsupportedDescriptor,
			TEXT("operation-unsupported"), Resources);
		const auto Targets = MakeTargets(*this, *UnsupportedDescriptor,
			TEXT("rig.ik.definition.update"), TEXT("ik://unsupported"));
		const TSharedRef<FUeAgentRigSimulationSpatialOperationChain>* Chain =
			Runtime->GetOperationChains().FindByPredicate(
				[](const auto& Candidate)
				{
					return Candidate->GetContract().GetCapabilityId()
						== TEXT("rig.ik.definition.update");
				});
		TSharedPtr<const FUeAgentRigSimulationSpatialRequest> Typed;
		FRigSimulationSpatialRevision UnsupportedRevision;
		Runtime->TryReadRevision(TEXT("ik://unsupported"), UnsupportedRevision, Error);
		const FString Input = FString::Printf(TEXT(
			"{\"ik_rig_ref\":\"ik://unsupported\",\"definition_changes\":["
			"{\"kind\":\"retarget_root\",\"operation\":\"set\","
			"\"primary_ref\":\"bone://root\"}],\"expected_revision\":{"
			"\"source_id\":\"%s\",\"generation\":\"%s\",\"sequence\":\"%lld\"}}"),
			*UnsupportedRevision.SourceId, *UnsupportedRevision.Generation,
			UnsupportedRevision.Sequence);
		FUeAgentOperationBindingError BindingError;
		TOptional<FPlanningRequest> PlanningRequest;
		TestFalse(TEXT("unimplemented families remain fail-closed"),
			Chain && Context.IsValid() && (*Chain)->TryBuildPlanningRequest(
				FUeAgentContractValue(Input), Targets, *Context,
				PlanningRequest, BindingError));
		TestEqual(TEXT("fail-closed code is stable"), BindingError.Code,
			TEXT("rig_simulation_spatial_family_port_unavailable"));
	}
	return !HasAnyErrors();
}

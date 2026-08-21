#include "Misc/AutomationTest.h"

#include "ControlRigBlueprint.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "DomainAdapters/AnimationRigGraph/UeAgentAnimationRigGraphCandidateBindingNormalizer.h"
#include "DomainAdapters/AnimationRigGraph/UeAgentAnimationRigGraphNativeHook.h"
#include "DomainAdapters/AudioGraph/UeAgentAudioGraphCandidateBindingNormalizer.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/SkeletalMesh.h"
#include "QueryOwners/AnimationRigGraph/UeAgentAnimationRigGraphCandidateQuery.h"
#include "QueryOwners/AudioGraph/UeAgentAudioGraphCandidateQuery.h"
#include "ReferenceSkeleton.h"
#include "Retargeter/IKRetargeter.h"
#include "Rig/IKRigDefinition.h"
#include "RigEditor/IKRigController.h"
#include "RigVMModel/Nodes/RigVMUnitNode.h"
#include "RigVMModel/RigVMGraph.h"
#include "Sound/SoundCue.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	using namespace UE::UAI;

	TArray<TSharedPtr<FJsonValue>> StringValues(
		const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		for (const FString& Value : Values)
		{
			Result.Add(MakeShared<FJsonValueString>(Value));
		}
		return Result;
	}

	TSharedRef<FJsonObject> AudioCandidateJson(
		const QueryOwners::AudioGraph::FAudioGraphCandidate& Candidate)
	{
		const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("target_ref"), Candidate.TargetRef);
		Result->SetStringField(
			TEXT("target_revision"), Candidate.TargetRevision);
		Result->SetStringField(TEXT("name"), Candidate.Name);
		Result->SetStringField(TEXT("kind"), Candidate.Kind);
		Result->SetArrayField(
			TEXT("supported_operations"),
			StringValues(Candidate.SupportedOperations));
		Result->SetStringField(
			TEXT("input_contract"), Candidate.InputContract);
		Result->SetStringField(
			TEXT("placement_contract"), Candidate.PlacementContract);
		Result->SetStringField(
			TEXT("required_context"), Candidate.RequiredContext);
		Result->SetStringField(
			TEXT("binding_revision"), Candidate.BindingRevision);
		if (Candidate.Kind == TEXT("metasound_node"))
		{
			Result->SetNumberField(
				TEXT("major_version"), Candidate.MajorVersion);
			Result->SetNumberField(
				TEXT("minor_version"), Candidate.MinorVersion);
		}
		return Result;
	}

	TSharedRef<FJsonObject> RigCandidateJson(
		const QueryOwners::AnimationRigGraph::FAnimationRigGraphCandidate&
			Candidate)
	{
		const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("target_ref"), Candidate.TargetRef);
		Result->SetStringField(
			TEXT("target_revision"), Candidate.TargetRevision);
		Result->SetStringField(TEXT("name"), Candidate.Name);
		Result->SetStringField(TEXT("kind"), Candidate.Kind);
		Result->SetArrayField(
			TEXT("supported_operations"),
			StringValues(Candidate.SupportedOperations));
		Result->SetStringField(
			TEXT("input_contract"), Candidate.InputContract);
		Result->SetStringField(
			TEXT("placement_contract"), Candidate.PlacementContract);
		Result->SetStringField(
			TEXT("required_context"), Candidate.RequiredContext);
		Result->SetStringField(
			TEXT("binding_revision"), Candidate.BindingRevision);
		return Result;
	}

	TSharedRef<FJsonObject> AudioNativeParams(
		const TSharedRef<FJsonObject>& Candidate,
		const FString& ObjectPath,
		const FString& ExistingClass = FString())
	{
		const TSharedRef<FJsonObject> Node = MakeShared<FJsonObject>();
		Node->SetStringField(TEXT("intent"), TEXT("create"));
		Node->SetStringField(TEXT("object_path"), ObjectPath);
		Node->SetObjectField(TEXT("candidate"), Candidate);
		if (!ExistingClass.IsEmpty())
		{
			Node->SetStringField(TEXT("native_class_ref"), ExistingClass);
		}
		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetArrayField(
			TEXT("nodes"), {MakeShared<FJsonValueObject>(Node)});
		return Params;
	}

	TSharedRef<FJsonObject> RigNativeParams(
		const TSharedRef<FJsonObject>& Candidate,
		const FString& ExistingStruct = FString())
	{
		const TSharedRef<FJsonObject> Position = MakeShared<FJsonObject>();
		Position->SetNumberField(TEXT("x"), 120.0);
		Position->SetNumberField(TEXT("y"), -40.0);
		const TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
		Value->SetObjectField(TEXT("candidate"), Candidate);
		Value->SetObjectField(TEXT("position"), Position);
		if (!ExistingStruct.IsEmpty())
		{
			Value->SetStringField(TEXT("script_struct_ref"), ExistingStruct);
		}
		const TSharedRef<FJsonObject> Edit = MakeShared<FJsonObject>();
		Edit->SetStringField(TEXT("intent"), TEXT("create"));
		Edit->SetStringField(TEXT("semantic_id"), TEXT("rigvm_node:T40Unit"));
		Edit->SetObjectField(TEXT("value"), Value);
		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetArrayField(
			TEXT("edits"), {MakeShared<FJsonValueObject>(Edit)});
		return Params;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT40DomainAuthoringMultiRootContractTest,
	"UAI.Root.T40.AnimationRigAudio.DomainAuthoringMultiRootContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT40DomainAuthoringMultiRootContractTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI::HjsonWorkspace::DomainAuthoring;
	const FDomainHjsonDocumentContract Contract(
		TEXT("audio_graph"),
		TEXT("uai.audio_graph.edit/1"),
		TArray<FString>{
			TEXT("/Script/MetasoundEngine.MetaSoundSource"),
			TEXT("/Script/Engine.SoundCue"),
			TEXT("/Script/Engine.SoundCue")});
	TestTrue(TEXT("a multi-root contract is valid"), Contract.IsValid());
	TestTrue(
		TEXT("MetaSound is accepted"),
		Contract.SupportsRootType(
			TEXT("/Script/MetasoundEngine.MetaSoundSource")));
	TestTrue(
		TEXT("SoundCue is accepted"),
		Contract.SupportsRootType(TEXT("/Script/Engine.SoundCue")));
	TestFalse(
		TEXT("an unrelated root is rejected"),
		Contract.SupportsRootType(TEXT("/Script/Engine.SoundWave")));
	TestEqual(
		TEXT("duplicate root identities are removed"),
		Contract.RootTypeIdentities.Num(),
		2);

	HjsonWorkspace::FWorkspaceError PropagatedError;
	SetWorkspaceErrorFromCommandFailure(
		TEXT("{\"code\":\"contract_input_schema_mismatch\","
			"\"detail\":\"invalid /chain_mappings\","
			"\"subject_id\":\"animation.ik_retargeter.definition.apply\"}"),
		TEXT("fallback_native_command_failed"),
		TEXT("fallback.command"),
		PropagatedError);
	TestEqual(
		TEXT("the native command error code remains actionable"),
		PropagatedError.GetCode(),
		FString(TEXT("contract_input_schema_mismatch")));
	TestEqual(
		TEXT("the native command subject remains exact"),
		PropagatedError.GetSourceUnit(),
		FString(TEXT("animation.ik_retargeter.definition.apply")));
	TestEqual(
		TEXT("the native command detail remains inspectable"),
		PropagatedError.GetJsonPointer(),
		FString(TEXT("invalid /chain_mappings")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT40AudioCandidateBindingNormalizationTest,
	"UAI.Root.T40.AnimationRigAudio.AudioCandidateBindingNormalization",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT40AudioCandidateBindingNormalizationTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI;
	using namespace DomainAdapters::AudioGraph;
	using namespace QueryOwners::AudioGraph;
	TStrongObjectPtr<USoundCue> Cue(NewObject<USoundCue>(
		GetTransientPackage(),
		*FString(TEXT("SC_T40_Candidate_")).Append(
			FGuid::NewGuid().ToString(EGuidFormats::Digits)),
		RF_Transient));
	if (!TestNotNull(TEXT("a transient SoundCue is available"), Cue.Get()))
	{
		return false;
	}
	FAudioGraphCandidateQueryResult Query;
	FAudioGraphCandidateQueryError QueryError;
	if (!TestTrue(
			TEXT("the exact SoundNodeMixer candidate is queried"),
			TryQueryAudioGraphCandidates(
				*Cue, TEXT("SoundNodeMixer"), Query, QueryError, 64, 0))
		|| !TestTrue(TEXT("the query returns a candidate"), !Query.Candidates.IsEmpty()))
	{
		AddError(QueryError.Code + TEXT(":") + QueryError.Detail);
		return false;
	}
	const FAudioGraphCandidate Candidate = Query.Candidates[0];
	HjsonWorkspace::FWorkspaceError WorkspaceError;
	const FString NodePath = Cue->GetPathName() + TEXT(":T40Node");
	const TSharedRef<FJsonObject> Params = AudioNativeParams(
		AudioCandidateJson(Candidate), NodePath);
	if (!TestTrue(
			TEXT("a complete candidate normalizes before mutation"),
			TryNormalizeAudioGraphCandidateBindings(
				Cue->GetPathName(), Params, WorkspaceError)))
	{
		AddError(WorkspaceError.GetCode());
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>& Nodes =
		Params->GetArrayField(TEXT("nodes"));
	const TSharedPtr<FJsonObject> Node = Nodes[0]->AsObject();
	TestEqual(
		TEXT("the native class comes from the candidate"),
		Node->GetStringField(TEXT("native_class_ref")),
		Candidate.Name);
	TestFalse(
		TEXT("the transport-only candidate is removed"),
		Node->HasField(TEXT("candidate")));

	const TSharedRef<FJsonObject> Stale = AudioCandidateJson(Candidate);
	Stale->SetStringField(TEXT("binding_revision"), TEXT("sha256:stale"));
	WorkspaceError.Reset();
	TestFalse(
		TEXT("a stale Audio candidate is rejected before mutation"),
		TryNormalizeAudioGraphCandidateBindings(
			Cue->GetPathName(),
			AudioNativeParams(Stale, NodePath),
			WorkspaceError));
	TestEqual(
		TEXT("the stale error is typed"),
		WorkspaceError.GetCode(),
		FString(TEXT("audio_graph_candidate_binding_stale")));

	WorkspaceError.Reset();
	TestFalse(
		TEXT("a conflicting native class is rejected"),
		TryNormalizeAudioGraphCandidateBindings(
			Cue->GetPathName(),
			AudioNativeParams(
				AudioCandidateJson(Candidate),
				NodePath,
				TEXT("/Script/Engine.SoundNodeWavePlayer")),
			WorkspaceError));
	TestEqual(
		TEXT("the conflict error is typed"),
		WorkspaceError.GetCode(),
		FString(TEXT("audio_graph_candidate_normalization_conflict")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT40RigCandidateAndRigVmUnitTest,
	"UAI.Root.T40.AnimationRigAudio.RigCandidateAndRigVmUnit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT40RigCandidateAndRigVmUnitTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI;
	using namespace DomainAdapters::AnimationRigGraph;
	using namespace QueryOwners::AnimationRigGraph;
	TStrongObjectPtr<UControlRigBlueprint> Rig(NewObject<UControlRigBlueprint>(
		GetTransientPackage(),
		*FString(TEXT("CR_T40_Candidate_")).Append(
			FGuid::NewGuid().ToString(EGuidFormats::Digits)),
		RF_Transient | RF_Transactional));
	if (!TestNotNull(TEXT("a transient Control Rig is available"), Rig.Get()))
	{
		return false;
	}
	FAnimationRigGraphCandidateQueryResult Query;
	FAnimationRigGraphCandidateQueryError QueryError;
	if (!TestTrue(
			TEXT("a current RigUnit candidate is queried"),
			TryQueryAnimationRigGraphCandidates(
				*Rig, TEXT("BeginExecution"), Query, QueryError, 64, 0))
		|| !TestTrue(TEXT("the RigUnit query returns a candidate"),
			!Query.Candidates.IsEmpty()))
	{
		AddError(QueryError.Code + TEXT(":") + QueryError.Detail);
		return false;
	}
	const FAnimationRigGraphCandidate Candidate = Query.Candidates[0];
	HjsonWorkspace::FWorkspaceError WorkspaceError;
	const TSharedRef<FJsonObject> Params = RigNativeParams(
		RigCandidateJson(Candidate));
	if (!TestTrue(
			TEXT("a complete RigUnit candidate normalizes before mutation"),
			TryNormalizeAnimationRigGraphCandidateBindings(
				Rig->GetPathName(), Params, WorkspaceError)))
	{
		AddError(WorkspaceError.GetCode());
		return false;
	}
	const TSharedPtr<FJsonObject> Edit =
		Params->GetArrayField(TEXT("edits"))[0]->AsObject();
	const TSharedPtr<FJsonObject> Value =
		Edit->GetObjectField(TEXT("value"));
	TestEqual(
		TEXT("the semantic identity is normalized for the native graph contract"),
		Edit->GetStringField(TEXT("identity")),
		FString(TEXT("rigvm_node:T40Unit")));
	TestEqual(
		TEXT("the RigVM unit is normalized as a native graph item"),
		Edit->GetStringField(TEXT("element_kind")),
		FString(TEXT("item")));
	TestFalse(
		TEXT("the transport-only semantic_id is removed"),
		Edit->HasField(TEXT("semantic_id")));
	TestEqual(
		TEXT("the RigVM struct comes from the candidate"),
		Value->GetStringField(TEXT("script_struct_ref")),
		Candidate.Name);
	TestEqual(
		TEXT("the default unit method is explicit"),
		Value->GetStringField(TEXT("method")),
		FString(TEXT("Execute")));
	TestFalse(
		TEXT("the transport-only Rig candidate is removed"),
		Value->HasField(TEXT("candidate")));

	const TSharedRef<FJsonObject> Stale = RigCandidateJson(Candidate);
	Stale->SetStringField(TEXT("binding_revision"), TEXT("sha256:stale"));
	WorkspaceError.Reset();
	TestFalse(
		TEXT("a stale Rig candidate is rejected before mutation"),
		TryNormalizeAnimationRigGraphCandidateBindings(
			Rig->GetPathName(), RigNativeParams(Stale), WorkspaceError));
	TestEqual(
		TEXT("the Rig stale error is typed"),
		WorkspaceError.GetCode(),
		FString(TEXT("animation_rig_graph_candidate_binding_stale")));

	FAnimationRigGraphNativeHook Hook;
	FAnimationRigGraphNativeMutationReceipt Apply;
	FAnimationRigGraphBarrierReceipt Barrier;
	FAnimationRigGraphTopology Topology;
	FString Revision;
	FString Evidence;
	FAnimationRigGraphAdapterError NativeError;
	if (!TestTrue(
			TEXT("the Control Rig owns a compile-ready model"),
			Hook.TryEnsureCompileAndCaptureControlRig(
				*Rig,
				TEXT("root"),
				TEXT("T40Control"),
				Apply,
				Barrier,
				Topology,
				Revision,
				Evidence,
				NativeError)))
	{
		AddError(NativeError.GetCode() + TEXT(":") + NativeError.GetDetail());
		return false;
	}
	FAnimationRigGraphUnitMutation Unit;
	Unit.NodeName = TEXT("T40Unit");
	Unit.ScriptStructRef = Candidate.Name;
	Unit.MethodName = TEXT("Execute");
	Unit.Position = FVector2D(120.0, -40.0);
	if (!TestTrue(
			TEXT("the current native owner creates and captures a RigVM unit"),
			Hook.TryApplyAndCaptureControlRigDefinition(
				*Rig,
				{},
				{Unit},
				Apply,
				Barrier,
				Topology,
				Revision,
				Evidence,
				NativeError)))
	{
		AddError(NativeError.GetCode() + TEXT(":") + NativeError.GetDetail());
		return false;
	}
	TestTrue(
		TEXT("the topology contains the exact RigVM unit identity"),
		Topology.GetItems().ContainsByPredicate(
			[](const FAnimationRigGraphItemIdentity& Item)
			{
				return Item.GetItemKey() == TEXT("rigvm_node:T40Unit")
					&& Item.GetSemanticRole() == TEXT("rig_unit");
			}));
	const URigVMGraph* const Graph = Rig->GetDefaultModel();
	const URigVMUnitNode* PersistedUnit = nullptr;
	if (Graph)
	{
		for (const URigVMNode* Node : Graph->GetNodes())
		{
			if (Node && Node->GetName() == TEXT("T40Unit"))
			{
				PersistedUnit = Cast<URigVMUnitNode>(Node);
				break;
			}
		}
	}
	TestNotNull(TEXT("the unit exists in the actual RigVM model"), PersistedUnit);
	if (PersistedUnit)
	{
		TestEqual(
			TEXT("the unit keeps the exact script struct"),
			PersistedUnit->GetScriptStruct()->GetPathName(),
			Candidate.Name);
		TestTrue(
			TEXT("the unit keeps its two-dimensional placement"),
			PersistedUnit->GetPosition().Equals(Unit.Position, 0.01));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT40RigIkContentGraphStructureReadbackTest,
	"UAI.Root.T40.AnimationRigAudio.RigIkContentGraphStructureReadback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT40RigIkContentGraphStructureReadbackTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI;
	using namespace DomainAdapters::AnimationRigGraph;
	using namespace DomainAdapters::ContentGraphRead;
	using namespace HjsonWorkspace::DomainAuthoring;

	auto NewRig = []()
	{
		return TStrongObjectPtr<UIKRigDefinition>(NewObject<UIKRigDefinition>(
			GetTransientPackage(),
			*FString(TEXT("IKR_T40_Structure_")).Append(
				FGuid::NewGuid().ToString(EGuidFormats::Digits)),
			RF_Transactional));
	};
	TStrongObjectPtr<UIKRigDefinition> SourceRig = NewRig();
	TStrongObjectPtr<UIKRigDefinition> TargetRig = NewRig();
	USkeletalMesh* const Mesh = LoadObject<USkeletalMesh>(
		nullptr, TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
	if (!TestNotNull(TEXT("a transient source IKRig is available"), SourceRig.Get())
		|| !TestNotNull(TEXT("a transient target IKRig is available"), TargetRig.Get())
		|| !TestNotNull(TEXT("a native skeletal mesh is available"), Mesh)
		|| !TestTrue(TEXT("the native mesh exposes a bone chain"),
			Mesh && Mesh->GetRefSkeleton().GetNum() >= 2))
	{
		return false;
	}
	const FReferenceSkeleton& ReferenceSkeleton = Mesh->GetRefSkeleton();
	const FName StartBone = ReferenceSkeleton.GetBoneName(0);
	const FName EndBone = ReferenceSkeleton.GetBoneName(
		ReferenceSkeleton.GetNum() - 1);
	UIKRigController* const SourceController =
		UIKRigController::GetController(SourceRig.Get());
	UIKRigController* const TargetController =
		UIKRigController::GetController(TargetRig.Get());
	if (!TestNotNull(TEXT("the source IKRig controller is available"),
			SourceController)
		|| !TestNotNull(TEXT("the target IKRig controller is available"),
			TargetController)
		|| !TestTrue(TEXT("the source IKRig owns the native hierarchy"),
			SourceController && SourceController->SetSkeletalMesh(Mesh))
		|| !TestTrue(TEXT("the target IKRig owns the native hierarchy"),
			TargetController && TargetController->SetSkeletalMesh(Mesh)))
	{
		return false;
	}

	FAnimationRigGraphNativeHook Hook;
	FAnimationRigGraphNativeMutationReceipt Apply;
	FAnimationRigGraphBarrierReceipt Barrier;
	FAnimationRigGraphTopology Topology;
	FString Revision;
	FString Evidence;
	FAnimationRigGraphAdapterError NativeError;
	for (UIKRigDefinition* Rig : {SourceRig.Get(), TargetRig.Get()})
	{
		if (!TestTrue(
				TEXT("the native owner creates an exact IK chain"),
				Hook.TryConfigureValidateAndCaptureIKRig(
					*Rig,
					TEXT("T40Goal"),
					EndBone,
					TEXT("T40Chain"),
					StartBone,
					EndBone,
					Apply,
					Barrier,
					Topology,
					Revision,
					Evidence,
					NativeError)))
		{
			AddError(NativeError.GetCode() + TEXT(":") + NativeError.GetDetail());
			return false;
		}
	}

	FUeAgentContentGraphCapture RigCapture;
	FUeAgentContentGraphReadError CaptureError;
	if (!TestTrue(
			TEXT("ContentGraph captures non-UObject IKRig topology"),
			FUeAgentContentGraphReadProductionEnvironment::TryCaptureOwner(
				*SourceRig, RigCapture, CaptureError)))
	{
		AddError(CaptureError.Code + TEXT(":") + CaptureError.Detail);
		return false;
	}
	const FUeAgentContentGraphItem* ChainItem =
		RigCapture.Items.FindByPredicate(
			[](const FUeAgentContentGraphItem& Item)
			{
				return Item.ItemId == TEXT("hierarchy:ik_chain:T40Chain");
			});
	TestNotNull(TEXT("the exact IK chain is publicly addressable"), ChainItem);
	if (ChainItem)
	{
		TestTrue(TEXT("the IK chain carries native structure state"),
			ChainItem->State.IsValid());
	}

	TSharedPtr<FJsonObject> RigSnapshot;
	HjsonWorkspace::FWorkspaceError WorkspaceError;
	if (!TestTrue(
			TEXT("the HJSON readback snapshot preserves IK structure state"),
			TryBuildCaptureSnapshot(
				SourceRig->GetPathName(),
				SourceRig->GetClass()->GetPathName(),
				RigCapture,
				RigSnapshot,
				WorkspaceError)))
	{
		AddError(WorkspaceError.GetCode());
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>& RigItems =
		RigSnapshot->GetArrayField(TEXT("items"));
	TSharedPtr<FJsonObject> SnapshotChain;
	for (const TSharedPtr<FJsonValue>& Item : RigItems)
	{
		const TSharedPtr<FJsonObject> Object =
			Item.IsValid() ? Item->AsObject() : nullptr;
		if (Object.IsValid()
			&& Object->GetStringField(TEXT("id"))
				== TEXT("hierarchy:ik_chain:T40Chain"))
		{
			SnapshotChain = Object;
			break;
		}
	}
	TestNotNull(
		TEXT("the IK chain survives snapshot projection"),
		SnapshotChain.Get());
	if (SnapshotChain.IsValid())
	{
		const TSharedPtr<FJsonObject> Value =
			SnapshotChain->GetObjectField(TEXT("value"));
		TestEqual(TEXT("the chain start bone is independently readable"),
			Value->GetStringField(TEXT("start_bone")),
			StartBone.ToString());
		TestEqual(TEXT("the chain end bone is independently readable"),
			Value->GetStringField(TEXT("end_bone")),
			EndBone.ToString());
	}

	TStrongObjectPtr<UIKRetargeter> Retargeter(NewObject<UIKRetargeter>(
		GetTransientPackage(),
		*FString(TEXT("RTG_T40_Structure_")).Append(
			FGuid::NewGuid().ToString(EGuidFormats::Digits)),
		RF_Transactional));
	if (!TestNotNull(TEXT("a transient IKRetargeter is available"), Retargeter.Get())
		|| !TestTrue(
			TEXT("the native owner creates an exact retarget mapping"),
			Hook.TryConfigureValidateAndCaptureIKRetargeter(
				*Retargeter,
				*SourceRig,
				*TargetRig,
				TEXT("T40Chain"),
				Apply,
				Barrier,
				Topology,
				Revision,
				Evidence,
				NativeError)))
	{
		AddError(NativeError.GetCode() + TEXT(":") + NativeError.GetDetail());
		return false;
	}
	FUeAgentContentGraphCapture RetargetCapture;
	if (!TestTrue(
			TEXT("ContentGraph captures the retarget mapping"),
			FUeAgentContentGraphReadProductionEnvironment::TryCaptureOwner(
				*Retargeter, RetargetCapture, CaptureError)))
	{
		AddError(CaptureError.Code + TEXT(":") + CaptureError.Detail);
		return false;
	}
	const FUeAgentContentGraphItem* MappingItem =
		RetargetCapture.Items.FindByPredicate(
			[](const FUeAgentContentGraphItem& Item)
			{
				return Item.ItemId
					== TEXT("binding:chain_mapping:T40Chain");
			});
	TestNotNull(TEXT("the exact retarget mapping is publicly addressable"),
		MappingItem);
	if (MappingItem)
	{
		TestTrue(TEXT("the retarget mapping carries native structure state"),
			MappingItem->State.IsValid());
	}
	return !HasAnyErrors();
}

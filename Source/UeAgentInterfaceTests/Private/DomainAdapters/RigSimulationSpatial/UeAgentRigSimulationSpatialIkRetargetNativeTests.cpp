#include "Misc/AutomationTest.h"

// RSP-004 is test-first. The full test is compiled only after the independent
// IK/Retarget native component and its public asset-effect seam both exist.
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Core/BindingGateway/Ticket/UeAgentBindingTicket.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialIkRetargetNative.h"
#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialProductionRuntime.h"
#include "Engine/SkeletalMesh.h"
#include "Modules/ModuleManager.h"
#include "RetargetEditor/IKRetargetBatchOperation.h"
#include "RetargetEditor/IKRetargeterController.h"
#include "Retargeter/IKRetargeter.h"
#include "Rig/IKRigDefinition.h"
#include "RigEditor/IKRigController.h"
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

	struct FFixture
	{
		USkeleton* SourceSkeleton = nullptr;
		USkeleton* TargetSkeleton = nullptr;
		USkeletalMesh* SourceMesh = nullptr;
		USkeletalMesh* TargetMesh = nullptr;
		UIKRigDefinition* SourceRig = nullptr;
		UIKRigDefinition* TargetRig = nullptr;
		UIKRetargeter* Retargeter = nullptr;
		UAnimSequence* SourceAnimation = nullptr;
		UAnimSequence* ExistingGeneratedAnimation = nullptr;
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

	TSharedPtr<FJsonObject> RevisionJson(
		const FRigSimulationSpatialRevision& Revision)
	{
		const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("source_id"), Revision.SourceId);
		Result->SetStringField(TEXT("generation"), Revision.Generation);
		Result->SetStringField(TEXT("sequence"), LexToString(Revision.Sequence));
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

	TSharedPtr<FJsonObject> FrameJson()
	{
		const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("frame_id"), TEXT("frame://pose-requested"));
		Result->SetStringField(TEXT("basis_frame_id"), TEXT("frame://basis"));
		Result->SetNumberField(TEXT("dimension"), 3);
		Result->SetStringField(TEXT("unit"), TEXT("cm"));
		Result->SetStringField(TEXT("basis_evidence_id"), TEXT("basis://rsp004"));
		return Result;
	}

	TSharedPtr<FJsonObject> PoseTransformJson()
	{
		const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("subject"), TEXT("bone://root"));
		Result->SetStringField(TEXT("quantity"), TEXT("transform"));
		Result->SetObjectField(TEXT("frame"), FrameJson());
		Result->SetArrayField(TEXT("components"), StringValues({
			TEXT("0"), TEXT("-1"), TEXT("0"), TEXT("0"),
			TEXT("1"), TEXT("0"), TEXT("0"), TEXT("0"),
			TEXT("0"), TEXT("0"), TEXT("1"), TEXT("3"),
			TEXT("0"), TEXT("0"), TEXT("0"), TEXT("1")}));
		Result->SetArrayField(TEXT("conversion_evidence_ids"),
			StringValues({TEXT("conversion://pose-request")}));
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
		if (Capability == TEXT("rig.ik.definition.update"))
		{
			Root->SetStringField(TEXT("ik_rig_ref"), TEXT("ik://source"));
			TArray<TSharedPtr<FJsonValue>> Changes;
			auto AddChange = [&Changes](const TCHAR* Kind, const TCHAR* Primary,
				const TCHAR* Secondary = TEXT(""), const TCHAR* Tertiary = TEXT(""))
			{
				const TSharedPtr<FJsonObject> Change = MakeShared<FJsonObject>();
				Change->SetStringField(TEXT("kind"), Kind);
				Change->SetStringField(TEXT("operation"), TEXT("set"));
				Change->SetStringField(TEXT("primary_ref"), Primary);
				if (*Secondary) Change->SetStringField(TEXT("secondary_ref"), Secondary);
				if (*Tertiary) Change->SetStringField(TEXT("tertiary_ref"), Tertiary);
				Changes.Add(MakeShared<FJsonValueObject>(Change));
			};
			AddChange(TEXT("preview_mesh"), TEXT("mesh://source"));
			AddChange(TEXT("retarget_root"), TEXT("bone://root"));
			AddChange(TEXT("goal"), TEXT("goal://root"), TEXT("bone://root"));
			AddChange(TEXT("retarget_chain"), TEXT("chain://root"),
				TEXT("bone://root"), TEXT("bone://root"));
			Root->SetArrayField(TEXT("definition_changes"), MoveTemp(Changes));
			const TSharedPtr<FJsonObject> Preview = MakeShared<FJsonObject>();
			Preview->SetStringField(TEXT("preview_mesh_ref"), TEXT("mesh://source"));
			Root->SetObjectField(TEXT("preview_context"), Preview);
		}
		else if (Capability == TEXT("rig.retarget.definition.update"))
		{
			Root->SetStringField(TEXT("retargeter_ref"), TEXT("retargeter://rsp004"));
			Root->SetStringField(TEXT("source_rig_ref"), TEXT("ik://source"));
			Root->SetStringField(TEXT("target_rig_ref"), TEXT("ik://target"));
			const TSharedPtr<FJsonObject> Mapping = MakeShared<FJsonObject>();
			Mapping->SetStringField(TEXT("source_chain_ref"), TEXT("chain://root"));
			Mapping->SetStringField(TEXT("target_chain_ref"), TEXT("chain://root"));
			Mapping->SetStringField(TEXT("goal_ref"), TEXT("goal://root"));
			Root->SetArrayField(TEXT("chain_mappings"),
				{MakeShared<FJsonValueObject>(Mapping)});
		}
		else if (Capability == TEXT("rig.retarget.pose.update"))
		{
			Root->SetStringField(TEXT("retargeter_ref"), TEXT("retargeter://rsp004"));
			Root->SetStringField(TEXT("subject_side"), TEXT("source"));
			const TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("bone_ref"), TEXT("bone://root"));
			Item->SetObjectField(TEXT("transform"), PoseTransformJson());
			Root->SetArrayField(TEXT("pose"), {MakeShared<FJsonValueObject>(Item)});
			Root->SetObjectField(TEXT("pose_frame"), FrameJson());
		}
		else if (Capability == TEXT("rig.retarget.pose.auto_align"))
		{
			Root->SetStringField(TEXT("retargeter_ref"), TEXT("retargeter://rsp004"));
			Root->SetStringField(TEXT("subject_side"), TEXT("target"));
			Root->SetStringField(TEXT("alignment_policy"), TEXT("root"));
			Root->SetArrayField(TEXT("bone_refs"),
				StringValues({TEXT("bone://root")}));
		}
		else if (Capability == TEXT("rig.retarget.assets.generate"))
		{
			Root->SetStringField(TEXT("source_subject_ref"),
				TEXT("animation://source"));
			Root->SetStringField(TEXT("target_subject_ref"), TEXT("mesh://target"));
			Root->SetStringField(TEXT("retargeter_ref"), TEXT("retargeter://rsp004"));
			Root->SetStringField(TEXT("destination_root"),
				TEXT("/Game/__UAI_RSP004/Generated"));
			const TSharedPtr<FJsonObject> Request = MakeShared<FJsonObject>();
			Request->SetStringField(TEXT("asset_kind"), TEXT("animation_sequence"));
			Request->SetStringField(TEXT("name"), TEXT("GeneratedAnimation"));
			Request->SetStringField(TEXT("overwrite_policy"), TEXT("replace"));
			Root->SetArrayField(TEXT("asset_requests"),
				{MakeShared<FJsonValueObject>(Request)});
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
		Input.ProjectInstanceId = TEXT("rsp004-project");
		Input.EditorInstanceId = TEXT("rsp004-editor");
		Input.StatePlane = Planes[0];
		Input.WorldType = EUeAgentExecutionWorldType::Preview;
		Input.WorldIdentity = TEXT("world://rsp004-owned");
		Input.AssetEditorIdentity = TEXT("asset-editor://none");
		Input.ViewportIdentity = TEXT("viewport://none");
		Input.ContextRevision = TEXT("rsp004-context-1");
		Input.bResolvedFromCurrent = false;
		FUeAgentExecutionContextRefPtr Execution;
		FUeAgentExecutionContextError ExecutionError;
		if (!Test.TestTrue(TEXT("freeze explicit IK/Retarget context"),
			FUeAgentExecutionContextRef::TryCreate(Input, DescriptorValue.GetIdentity(),
				EUeAgentRequestKind::Operation,
				DescriptorValue.GetExecutionContextPolicy(), Execution, ExecutionError)))
			return nullptr;
		const FUeAgentCallerContext Caller(TEXT("rsp004-native-e2"),
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
			TEXT("rsp004-binding:") + Capability,
			FUeAgentCandidateSummary(
				FUeAgentCanonicalIdentity(TEXT("rsp004-target:") + Capability,
					TEXT("rig_simulation_spatial_target"), TEXT("rsp004-owner"),
					PublicRef, TEXT("target"), TEXT("rsp004-generation")),
				DescriptorValue.GetIdentity(), TEXT("rig_simulation_spatial_target"),
				PublicRef, TEXT("owned RSP-004 fixture"),
				{TEXT("RigSimulationSpatial"), TEXT("Fixture")}),
			FUeAgentCatalogRevision(TEXT("rsp004-catalog"),
				TEXT("rsp004-generation"), 1), PublicRef,
			TEXT("rig-simulation-spatial-native"),
			{EUeAgentCandidateOperation::Read, EUeAgentCandidateOperation::Update},
			TEXT("rsp004-native-binding"));
		TSharedPtr<const FUeAgentOperationTicketService> Service;
		FUeAgentOperationTicketError Error;
		if (!Test.TestTrue(TEXT("create operation ticket service"),
			FUeAgentOperationTicketService::TryCreate(TEXT("rsp004-integrity"), 60.0,
				[](){ return 100.0; }, [](){ return TEXT("rsp004-ticket"); },
				Service, Error))) return {};
		const FUeAgentOperationSessionIdentity Session(TEXT("rsp004-session"), 1,
			TEXT("sha256:0000000000000000000000000000000000000000000000000000000000000000"));
		const FUeAgentCallerContext Caller(TEXT("rsp004-native-e2"),
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
			else Test.AddError(Capability + TEXT(":execute:") + Error.Code);
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

	bool CreateFixture(FAutomationTestBase& Test, FFixture& Out)
	{
		USkeletalMesh* TemplateMesh = LoadObject<USkeletalMesh>(nullptr,
			TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
		if (!Test.TestNotNull(TEXT("load real skeletal mesh"), TemplateMesh)
			|| !Test.TestNotNull(TEXT("load real skeleton"),
				TemplateMesh ? TemplateMesh->GetSkeleton() : nullptr)) return false;
		UPackage* SourcePackage = CreatePackage(TEXT("/Game/__UAI_RSP004/SourceRigAssets"));
		UPackage* TargetPackage = CreatePackage(TEXT("/Game/__UAI_RSP004/TargetRigAssets"));
		Out.SourceSkeleton = DuplicateObject<USkeleton>(TemplateMesh->GetSkeleton(),
			SourcePackage, TEXT("SourceSkeleton"));
		Out.SourceMesh = DuplicateObject<USkeletalMesh>(TemplateMesh,
			SourcePackage, TEXT("SourceMesh"));
		Out.TargetSkeleton = DuplicateObject<USkeleton>(TemplateMesh->GetSkeleton(),
			TargetPackage, TEXT("TargetSkeleton"));
		Out.TargetMesh = DuplicateObject<USkeletalMesh>(TemplateMesh,
			TargetPackage, TEXT("TargetMesh"));
		if (!Out.SourceSkeleton || !Out.SourceMesh || !Out.TargetSkeleton
			|| !Out.TargetMesh) return false;
		Out.SourceMesh->SetSkeleton(Out.SourceSkeleton);
		Out.TargetMesh->SetSkeleton(Out.TargetSkeleton);
		Out.RootBone = Out.SourceSkeleton->GetReferenceSkeleton().GetBoneName(0);

		Out.SourceRig = NewObject<UIKRigDefinition>(SourcePackage, TEXT("SourceRig"),
			RF_Public | RF_Standalone | RF_Transactional);
		Out.TargetRig = NewObject<UIKRigDefinition>(TargetPackage, TEXT("TargetRig"),
			RF_Public | RF_Standalone | RF_Transactional);
		UIKRigController* SourceController = UIKRigController::GetController(Out.SourceRig);
		UIKRigController* TargetController = UIKRigController::GetController(Out.TargetRig);
		if (!SourceController || !TargetController
			|| !SourceController->SetSkeletalMesh(Out.SourceMesh)
			|| !TargetController->SetSkeletalMesh(Out.TargetMesh)) return false;
		TargetController->SetRetargetRoot(Out.RootBone);
		TargetController->AddNewGoal(TEXT("RootGoal"), Out.RootBone);
		TargetController->AddRetargetChain(TEXT("RootChain"), Out.RootBone,
			Out.RootBone, TEXT("RootGoal"));

		UPackage* RetargetPackage = CreatePackage(TEXT("/Game/__UAI_RSP004/Retargeter"));
		Out.Retargeter = NewObject<UIKRetargeter>(RetargetPackage, TEXT("Retargeter"),
			RF_Public | RF_Standalone | RF_Transactional);
		UIKRetargeterController* RetargetController =
			UIKRetargeterController::GetController(Out.Retargeter);
		if (!RetargetController) return false;
		RetargetController->SetIKRig(ERetargetSourceOrTarget::Source, Out.SourceRig);
		RetargetController->SetIKRig(ERetargetSourceOrTarget::Target, Out.TargetRig);
		RetargetController->SetPreviewMesh(ERetargetSourceOrTarget::Source, Out.SourceMesh);
		RetargetController->SetPreviewMesh(ERetargetSourceOrTarget::Target, Out.TargetMesh);
		RetargetController->AddDefaultOps();
		RetargetController->SetRootOffsetInRetargetPose(FVector(0.0, 0.0, 50.0),
			ERetargetSourceOrTarget::Target);

		UPackage* AnimationPackage = CreatePackage(
			TEXT("/Game/__UAI_RSP004/SourceAnimation"));
		Out.SourceAnimation = NewObject<UAnimSequence>(AnimationPackage,
			TEXT("SourceAnimation"), RF_Public | RF_Standalone | RF_Transactional);
		Out.SourceAnimation->SetSkeleton(Out.SourceSkeleton);
		IAnimationDataController& AnimationController =
			Out.SourceAnimation->GetController();
		AnimationController.InitializeModel();
		AnimationController.SetFrameRate(FFrameRate(30, 1), false);
		AnimationController.SetNumberOfFrames(FFrameNumber(2), false);
		AnimationController.AddBoneCurve(Out.RootBone, false);
		AnimationController.SetBoneTrackKeys(Out.RootBone,
			{FVector3f::ZeroVector, FVector3f(10.0f, 0.0f, 0.0f),
				FVector3f(20.0f, 0.0f, 0.0f)},
			{FQuat4f::Identity, FQuat4f::Identity, FQuat4f::Identity},
			{FVector3f::OneVector, FVector3f::OneVector, FVector3f::OneVector}, false);
		AnimationController.NotifyPopulated();

		UPackage* ExistingPackage = CreatePackage(
			TEXT("/Game/__UAI_RSP004/Generated/GeneratedAnimation"));
		Out.ExistingGeneratedAnimation = NewObject<UAnimSequence>(ExistingPackage,
			TEXT("GeneratedAnimation"), RF_Public | RF_Standalone | RF_Transactional);
		Out.ExistingGeneratedAnimation->SetSkeleton(Out.SourceSkeleton);
		Out.ExistingGeneratedAnimation->GetController().InitializeModel();

		FAssetRegistryModule& Registry = FModuleManager::LoadModuleChecked<
			FAssetRegistryModule>(TEXT("AssetRegistry"));
		const TArray<UObject*> RegisteredAssets = {
			Out.SourceRig, Out.TargetRig, Out.Retargeter,
			Out.SourceAnimation, Out.ExistingGeneratedAnimation};
		for (UObject* Asset : RegisteredAssets)
		{
			Registry.AssetCreated(Asset);
		}
		return true;
	}

	class FRealRetargetAssetEffectPort final
		: public IUeAgentRigSimulationSpatialAssetEffectPort
	{
	public:
		bool TryApplyRetargetAssets(
			const FUeAgentRigSimulationSpatialRetargetAssetEffectRequest& Request,
			const FUeAgentOperationContext& Context,
			FUeAgentRigSimulationSpatialRetargetAssetEffectResult& OutResult,
			FUeAgentRigSimulationSpatialExecutionError& OutError) override
		{
			++CallCount;
			OutResult = {};
			OutError = {};
			UAnimSequence* Source = Cast<UAnimSequence>(Request.SourceSubject.Get());
			USkeletalMesh* Target = Cast<USkeletalMesh>(Request.TargetSubject.Get());
			UIKRetargeter* Retargeter = Request.Retargeter.Get();
			UIKRetargeterController* Controller = Retargeter
				? UIKRetargeterController::GetController(Retargeter) : nullptr;
			if (!Source || !Target || !Controller || Request.AssetRequests.Num() != 1)
			{
				OutError.Code = TEXT("rsp004_asset_effect_request_invalid");
				return false;
			}
			const FRigSimulationSpatialAssetRequest& Item = Request.AssetRequests[0];
			if (Item.AssetKind != TEXT("animation_sequence")
				|| Item.OverwritePolicy != TEXT("replace"))
			{
				OutError.Code = TEXT("rsp004_asset_effect_policy_invalid");
				return false;
			}
			bSawReplace = true;
			FIKRetargetBatchOperationContext Batch;
			Batch.AssetsToRetarget.Add(Source);
			Batch.SourceMesh = Controller->GetPreviewMesh(
				ERetargetSourceOrTarget::Source);
			Batch.TargetMesh = Target;
			Batch.IKRetargetAsset = Retargeter;
			Batch.NameRule.FolderPath = Request.DestinationRoot;
			Batch.NameRule.ReplaceFrom = Source->GetName();
			const FString TemporaryName = Item.Name + TEXT("__UAI_EFFECT_TEMP");
			Batch.NameRule.ReplaceTo = TemporaryName;
			// The public effect owner generates an independently verifiable temporary
			// asset first. It owns replacement only after generation succeeded.
			Batch.bOverwriteExistingFiles = false;
			Batch.bIncludeReferencedAssets = false;
			UIKRetargetBatchOperation* Operation =
				NewObject<UIKRetargetBatchOperation>(GetTransientPackage());
			Operation->RunRetarget(Batch);
			const FString TemporaryPath = Request.DestinationRoot + TEXT("/")
				+ TemporaryName + TEXT(".") + TemporaryName;
			UObject* Generated = FindObject<UObject>(nullptr, *TemporaryPath);
			if (!Generated)
			{
				OutError.Code = TEXT("rsp004_asset_effect_generated_asset_missing");
				OutError.Detail = TemporaryPath;
				return false;
			}
			const FString ObjectPath = Request.DestinationRoot + TEXT("/")
				+ Item.Name + TEXT(".") + Item.Name;
			UAnimSequence* GeneratedAnimation = Cast<UAnimSequence>(Generated);
			UAnimSequence* ExistingAnimation = FindObject<UAnimSequence>(nullptr,
				*ObjectPath);
			const IAnimationDataModel* GeneratedModel = GeneratedAnimation
				? GeneratedAnimation->GetDataModel() : nullptr;
			if (!GeneratedAnimation || !ExistingAnimation || !GeneratedModel)
			{
				OutError.Code = TEXT("rsp004_asset_effect_replace_target_invalid");
				OutError.Detail = ObjectPath;
				return false;
			}
			ExistingAnimation->Modify();
			ExistingAnimation->SetSkeleton(GeneratedAnimation->GetSkeleton());
			IAnimationDataController& ExistingController =
				ExistingAnimation->GetController();
			ExistingController.RemoveAllBoneTracks(false);
			ExistingController.SetFrameRate(GeneratedModel->GetFrameRate(), false);
			ExistingController.SetNumberOfFrames(
				FFrameNumber(GeneratedModel->GetNumberOfFrames()), false);
			TArray<FName> BoneTracks;
			GeneratedModel->GetBoneTrackNames(BoneTracks);
			for (const FName BoneTrack : BoneTracks)
			{
				TArray<FTransform> Transforms;
				GeneratedModel->GetBoneTrackTransforms(BoneTrack, Transforms);
				TArray<FVector3f> Positions;
				TArray<FQuat4f> Rotations;
				TArray<FVector3f> Scales;
				for (const FTransform& Transform : Transforms)
				{
					Positions.Add(FVector3f(Transform.GetTranslation()));
					Rotations.Add(FQuat4f(Transform.GetRotation()));
					Scales.Add(FVector3f(Transform.GetScale3D()));
				}
				ExistingController.AddBoneCurve(BoneTrack, false);
				ExistingController.SetBoneTrackKeys(
					BoneTrack, Positions, Rotations, Scales, false);
			}
			ExistingController.NotifyPopulated();
			ExistingAnimation->RefreshCacheData();
			Generated = ExistingAnimation;
			OutResult.GeneratedAssets.Add(Generated);
			OutResult.GeneratedAssetRefs.Add(ObjectPath);
			return true;
		}

		int32 CallCount = 0;
		bool bSawReplace = false;
	};

	enum class EScriptedAssetEffectScenario : uint8
	{
		OwnerRejected,
		CancelledNoChange,
		TimeoutStateUnknown,
		RollbackFailedStateUnknown,
		SuccessWithUnverifiedState,
	};

	class FScriptedRetargetAssetEffectPort final
		: public IUeAgentRigSimulationSpatialAssetEffectPort
	{
	public:
		explicit FScriptedRetargetAssetEffectPort(UAnimSequence* InExistingAsset)
			: ExistingAsset(InExistingAsset) {}

		bool TryApplyRetargetAssets(
			const FUeAgentRigSimulationSpatialRetargetAssetEffectRequest& Request,
			const FUeAgentOperationContext& Context,
			FUeAgentRigSimulationSpatialRetargetAssetEffectResult& OutResult,
			FUeAgentRigSimulationSpatialExecutionError& OutError) override
		{
			++CallCount;
			OutResult = {};
			OutError = {};
			switch (Scenario)
			{
			case EScriptedAssetEffectScenario::OwnerRejected:
				OutError.Code = TEXT("rig_simulation_spatial_asset_effect_owner_rejected");
				return false;
			case EScriptedAssetEffectScenario::CancelledNoChange:
				OutError.Code = TEXT("rig_simulation_spatial_asset_effect_cancelled_no_change");
				return false;
			case EScriptedAssetEffectScenario::TimeoutStateUnknown:
				OutError.Code = TEXT("rig_simulation_spatial_asset_effect_timeout_state_unknown");
				OutError.Detail = TEXT("reconcile_required");
				return false;
			case EScriptedAssetEffectScenario::RollbackFailedStateUnknown:
				OutError.Code = TEXT(
					"rig_simulation_spatial_asset_effect_rollback_failed_state_unknown");
				return false;
			case EScriptedAssetEffectScenario::SuccessWithUnverifiedState:
				break;
			}

			if (!Request.IsValid() || !ExistingAsset.IsValid())
			{
				OutError.Code = TEXT("rig_simulation_spatial_asset_effect_fixture_invalid");
				return false;
			}
			OutResult.GeneratedAssets.Add(ExistingAsset.Get());
			OutResult.GeneratedAssetRefs.Add(ExistingAsset->GetPathName());
			return OutResult.IsValid();
		}

		void SetScenario(const EScriptedAssetEffectScenario InScenario)
		{
			Scenario = InScenario;
			CallCount = 0;
		}

		int32 CallCount = 0;

	private:
		TWeakObjectPtr<UAnimSequence> ExistingAsset;
		EScriptedAssetEffectScenario Scenario =
			EScriptedAssetEffectScenario::OwnerRejected;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentRigSimulationSpatialIkRetargetNativeTest,
	"UeAgentInterface.DomainAdapters.RigSimulationSpatial.E2.ProductionNative.IkRetarget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentRigSimulationSpatialIkRetargetNativeTest::RunTest(
	const FString& Parameters)
{
	FFixture Fixture;
	if (!CreateFixture(*this, Fixture)) return false;
	const TSharedRef<FRealRetargetAssetEffectPort> AssetEffect =
		MakeShared<FRealRetargetAssetEffectPort>();
	const FUeAgentTemporalPoint ObservedAt(TEXT("clock://pose"), TEXT("0"),
		TEXT("30"), TEXT("1"), TEXT("authored"));
	const TArray<TArray<FString>> IdentityBasis = {
		{TEXT("1"), TEXT("0"), TEXT("0")},
		{TEXT("0"), TEXT("1"), TEXT("0")},
		{TEXT("0"), TEXT("0"), TEXT("1")},
	};

	FUeAgentRigSimulationSpatialProductionRuntimeConfig Config;
	Config.ProjectInstanceId = TEXT("rsp004-project");
	Config.EditorInstanceId = TEXT("rsp004-editor");
	Config.ServerSessionId = TEXT("rsp004-session");
	Config.RuntimeGeneration = TEXT("rsp004-generation");
	Config.RegistryEpoch = 1;
	Config.ObjectBindings = {
		{TEXT("ik://source"), Fixture.SourceRig},
		{TEXT("ik://target"), Fixture.TargetRig},
		{TEXT("mesh://source"), Fixture.SourceMesh},
		{TEXT("mesh://target"), Fixture.TargetMesh},
		{TEXT("retargeter://rsp004"), Fixture.Retargeter},
		{TEXT("animation://source"), Fixture.SourceAnimation},
		{TEXT("physics://unsupported"), GetTransientPackage()},
	};
	FUeAgentRigSimulationSpatialIkRigBinding SourceBinding;
	SourceBinding.Identity = TEXT("ik://source");
	SourceBinding.Rig = Fixture.SourceRig;
	SourceBinding.PreviewMeshIdentity = TEXT("mesh://source");
	SourceBinding.BoneBindings.Add(TEXT("bone://root"), Fixture.RootBone);
	SourceBinding.GoalBindings.Add(TEXT("goal://root"), TEXT("RootGoal"));
	SourceBinding.ChainBindings.Add(TEXT("chain://root"), TEXT("RootChain"));
	FUeAgentRigSimulationSpatialIkRigBinding TargetBinding;
	TargetBinding.Identity = TEXT("ik://target");
	TargetBinding.Rig = Fixture.TargetRig;
	TargetBinding.PreviewMeshIdentity = TEXT("mesh://target");
	TargetBinding.BoneBindings.Add(TEXT("bone://root"), Fixture.RootBone);
	TargetBinding.GoalBindings.Add(TEXT("goal://root"), TEXT("RootGoal"));
	TargetBinding.ChainBindings.Add(TEXT("chain://root"), TEXT("RootChain"));
	Config.IkRigBindings = {SourceBinding, TargetBinding};
	FUeAgentRigSimulationSpatialRetargeterBinding RetargeterBinding;
	RetargeterBinding.Identity = TEXT("retargeter://rsp004");
	RetargeterBinding.Retargeter = Fixture.Retargeter;
	RetargeterBinding.SourceRigIdentity = TEXT("ik://source");
	RetargeterBinding.TargetRigIdentity = TEXT("ik://target");
	RetargeterBinding.SourceNativePoseFrameId = TEXT("frame://pose-source-native");
	RetargeterBinding.TargetNativePoseFrameId = TEXT("frame://pose-target-native");
	Config.RetargeterBindings = {RetargeterBinding};
	Config.AssetEffectPort = AssetEffect;
	Config.FrameBindings.Add(FUeAgentRigSimulationSpatialFrameBinding(
		FUeAgentSpatialFrame(TEXT("frame://pose-requested"), TEXT("frame://basis"), 3,
			IdentityBasis, {TEXT("0"), TEXT("0"), TEXT("10")}, TEXT("cm"),
			TEXT("basis://rsp004"), ObservedAt), false));
	Config.FrameBindings.Add(FUeAgentRigSimulationSpatialFrameBinding(
		FUeAgentSpatialFrame(TEXT("frame://pose-source-native"), TEXT("frame://basis"), 3,
			IdentityBasis, {TEXT("0"), TEXT("0"), TEXT("0")}, TEXT("cm"),
			TEXT("basis://rsp004"), ObservedAt), true));
	Config.FrameBindings.Add(FUeAgentRigSimulationSpatialFrameBinding(
		FUeAgentSpatialFrame(TEXT("frame://pose-target-native"), TEXT("frame://basis"), 3,
			IdentityBasis, {TEXT("0"), TEXT("0"), TEXT("0")}, TEXT("cm"),
			TEXT("basis://rsp004"), ObservedAt), true));
	Config.SpatialConversions.Add(FUeAgentSpatialConversion(
		TEXT("conversion://pose-requested-to-source"), TEXT("frame://pose-requested"),
		TEXT("frame://pose-source-native"), 3, 3, IdentityBasis,
		{TEXT("0"), TEXT("0"), TEXT("10")}, ObservedAt,
		TEXT("conversion-evidence://pose-source")));

	TSharedPtr<FUeAgentRigSimulationSpatialProductionRuntime> Runtime;
	FUeAgentRigSimulationSpatialExecutionError Error;
	if (!TestTrue(TEXT("compose IK/Retarget production runtime"),
		FUeAgentRigSimulationSpatialProductionRuntime::TryCreate(Config, Runtime, Error))
		|| !Runtime.IsValid())
	{
		AddError(Error.Code + TEXT(":") + Error.Detail);
		return false;
	}
	const FUeAgentResourceTrackerOwner Resources = MakeResources();
	FRigSimulationSpatialRevision Revision;

	Runtime->TryReadRevision(TEXT("ik://source"), Revision, Error);
	ExecuteOperation(*this, *Runtime, TEXT("rig.ik.definition.update"),
		CanonicalInput(TEXT("rig.ik.definition.update"), Revision),
		TEXT("ik://source"), Resources);
	UIKRigController* SourceController = UIKRigController::GetController(Fixture.SourceRig);
	TestEqual(TEXT("IK preview mesh independently reads back"),
		SourceController->GetSkeletalMesh(), Fixture.SourceMesh);
	TestEqual(TEXT("IK retarget root independently reads back"),
		SourceController->GetRetargetRoot(), Fixture.RootBone);
	TestEqual(TEXT("IK goal independently reads back"),
		SourceController->GetBoneForGoal(TEXT("RootGoal")), Fixture.RootBone);
	TestTrue(TEXT("IK chain independently reads back"),
		SourceController->GetRetargetChains().ContainsByPredicate(
			[](const FBoneChain& Chain){ return Chain.ChainName == TEXT("RootChain"); }));

	Runtime->TryReadRevision(TEXT("retargeter://rsp004"), Revision, Error);
	ExecuteOperation(*this, *Runtime, TEXT("rig.retarget.definition.update"),
		CanonicalInput(TEXT("rig.retarget.definition.update"), Revision),
		TEXT("retargeter://rsp004"), Resources);
	UIKRetargeterController* RetargetController =
		UIKRetargeterController::GetController(Fixture.Retargeter);
	TestTrue(TEXT("source IK subject stays explicit"),
		RetargetController->GetIKRig(ERetargetSourceOrTarget::Source)
			== Fixture.SourceRig);
	TestTrue(TEXT("target IK subject stays explicit"),
		RetargetController->GetIKRig(ERetargetSourceOrTarget::Target)
			== Fixture.TargetRig);
	TestEqual(TEXT("chain mapping independently reads back"),
		RetargetController->GetSourceChain(TEXT("RootChain")), FName(TEXT("RootChain")));

	Runtime->TryReadRevision(TEXT("retargeter://rsp004"), Revision, Error);
	ExecuteOperation(*this, *Runtime, TEXT("rig.retarget.pose.update"),
		CanonicalInput(TEXT("rig.retarget.pose.update"), Revision),
		TEXT("retargeter://rsp004"), Resources);
	TestEqual(TEXT("source pose C4 root offset independently reads back"),
		RetargetController->GetRootOffsetInRetargetPose(
			ERetargetSourceOrTarget::Source), FVector(0.0, 0.0, 13.0), 0.001);
	TestTrue(TEXT("source pose rotation independently reads back"),
		RetargetController->GetRotationOffsetForRetargetPoseBone(Fixture.RootBone,
			ERetargetSourceOrTarget::Source).Equals(
				FQuat(FVector::UpVector, HALF_PI), 0.001));
	TestTrue(TEXT("source pose did not leak into target subject"),
		RetargetController->GetRootOffsetInRetargetPose(
			ERetargetSourceOrTarget::Target).Equals(FVector(0.0, 0.0, 50.0), 0.001));

	Runtime->TryReadRevision(TEXT("retargeter://rsp004"), Revision, Error);
	ExecuteOperation(*this, *Runtime, TEXT("rig.retarget.pose.auto_align"),
		CanonicalInput(TEXT("rig.retarget.pose.auto_align"), Revision),
		TEXT("retargeter://rsp004"), Resources);
	TestFalse(TEXT("target root auto-align changed independently readable pose"),
		RetargetController->GetRootOffsetInRetargetPose(
			ERetargetSourceOrTarget::Target).Equals(FVector(0.0, 0.0, 50.0), 0.001));
	TestTrue(TEXT("target auto-align did not rewrite source subject"),
		RetargetController->GetRootOffsetInRetargetPose(
			ERetargetSourceOrTarget::Source).Equals(FVector(0.0, 0.0, 13.0), 0.001));

	FUeAgentRigSimulationSpatialProductionRuntimeConfig NoEffectConfig = Config;
	NoEffectConfig.AssetEffectPort.Reset();
	TSharedPtr<FUeAgentRigSimulationSpatialProductionRuntime> NoEffectRuntime;
	TestTrue(TEXT("compose runtime without optional asset-effect owner"),
		FUeAgentRigSimulationSpatialProductionRuntime::TryCreate(
			NoEffectConfig, NoEffectRuntime, Error));
	FString NoEffectFailure;
	TestFalse(TEXT("generation fails closed without public asset-effect port"),
		ExecuteOperation(*this, *NoEffectRuntime,
			TEXT("rig.retarget.assets.generate"),
			CanonicalInput(TEXT("rig.retarget.assets.generate"), {}),
			TEXT("/Game/__UAI_RSP004/Generated"), Resources, &NoEffectFailure));
	TestEqual(TEXT("missing asset-effect owner failure is exact"), NoEffectFailure,
		FString(TEXT("rig_simulation_spatial_asset_effect_port_unavailable")));

	ExecuteOperation(*this, *Runtime, TEXT("rig.retarget.assets.generate"),
		CanonicalInput(TEXT("rig.retarget.assets.generate"), {}),
		TEXT("/Game/__UAI_RSP004/Generated"), Resources);
	UAnimSequence* Generated = FindObject<UAnimSequence>(nullptr,
		TEXT("/Game/__UAI_RSP004/Generated/GeneratedAnimation.GeneratedAnimation"));
	TestNotNull(TEXT("generated animation independently resolves by exact path"), Generated);
	TestEqual(TEXT("generated animation independently reads target skeleton"),
		Generated ? Generated->GetSkeleton() : nullptr, Fixture.TargetSkeleton);
	TestTrue(TEXT("generated animation independently reads retargeted bone keys"),
		Generated && Generated->GetDataModel()
			&& Generated->GetDataModel()->GetNumberOfKeys() > 0);
	TestEqual(TEXT("replace crossed public asset-effect port exactly once"),
		AssetEffect->CallCount, 1);
	TestTrue(TEXT("public asset-effect port observed replace policy"),
		AssetEffect->bSawReplace);

	FString FailureCode;
	FRigSimulationSpatialRevision UnsupportedRevision;
	Runtime->TryReadRevision(TEXT("physics://unsupported"), UnsupportedRevision, Error);
	const TSharedRef<FJsonObject> Unsupported = MakeShared<FJsonObject>();
	Unsupported->SetStringField(TEXT("physical_material_ref"),
		TEXT("physics://unsupported"));
	Unsupported->SetObjectField(TEXT("expected_revision"),
		RevisionJson(UnsupportedRevision));
	const TSharedPtr<FJsonObject> MaterialState = MakeShared<FJsonObject>();
	MaterialState->SetStringField(TEXT("friction"), TEXT("0.5"));
	Unsupported->SetObjectField(TEXT("material_state"), MaterialState);
	TestFalse(TEXT("unimplemented family still fails closed"),
		ExecuteOperation(*this, *Runtime, TEXT("physics.material.update"),
			JsonInput(Unsupported), TEXT("physics://unsupported"), Resources,
			&FailureCode));
	TestEqual(TEXT("unimplemented family failure is exact"), FailureCode,
		FString(TEXT("rig_simulation_spatial_family_port_unavailable")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentRigSimulationSpatialAssetEffectFailureSemanticsTest,
	"UeAgentInterface.DomainAdapters.RigSimulationSpatial.E2.CrossOwner.AssetEffectFailureSemantics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentRigSimulationSpatialAssetEffectFailureSemanticsTest::RunTest(
	const FString& Parameters)
{
	FFixture Fixture;
	if (!CreateFixture(*this, Fixture)) return false;
	const TSharedRef<FScriptedRetargetAssetEffectPort> AssetEffect =
		MakeShared<FScriptedRetargetAssetEffectPort>(
			Fixture.ExistingGeneratedAnimation);

	FUeAgentRigSimulationSpatialProductionRuntimeConfig Config;
	Config.ProjectInstanceId = TEXT("rsp007-asset-project");
	Config.EditorInstanceId = TEXT("rsp007-asset-editor");
	Config.ServerSessionId = TEXT("rsp007-asset-session");
	Config.RuntimeGeneration = TEXT("rsp007-asset-generation");
	Config.RegistryEpoch = 1;
	Config.ObjectBindings = {
		{TEXT("animation://source"), Fixture.SourceAnimation},
		{TEXT("mesh://target"), Fixture.TargetMesh},
		{TEXT("retargeter://rsp004"), Fixture.Retargeter},
	};
	Config.AssetEffectPort = AssetEffect;

	const struct
	{
		EScriptedAssetEffectScenario Scenario;
		const TCHAR* ErrorCode;
	} FailureCases[] = {
		{EScriptedAssetEffectScenario::OwnerRejected,
			TEXT("rig_simulation_spatial_asset_effect_owner_rejected")},
		{EScriptedAssetEffectScenario::CancelledNoChange,
			TEXT("rig_simulation_spatial_asset_effect_cancelled_no_change")},
		{EScriptedAssetEffectScenario::TimeoutStateUnknown,
			TEXT("rig_simulation_spatial_asset_effect_timeout_state_unknown")},
		{EScriptedAssetEffectScenario::RollbackFailedStateUnknown,
			TEXT("rig_simulation_spatial_asset_effect_rollback_failed_state_unknown")},
	};
	for (const auto& FailureCase : FailureCases)
	{
		AssetEffect->SetScenario(FailureCase.Scenario);
		TSharedPtr<FUeAgentRigSimulationSpatialProductionRuntime> Runtime;
		FUeAgentRigSimulationSpatialExecutionError Error;
		if (!TestTrue(TEXT("compose asset-effect boundary runtime"),
			FUeAgentRigSimulationSpatialProductionRuntime::TryCreate(
				Config, Runtime, Error)) || !Runtime.IsValid()) return false;
		const USkeleton* SkeletonBefore =
			Fixture.ExistingGeneratedAnimation->GetSkeleton();
		const int32 KeysBefore = Fixture.ExistingGeneratedAnimation->GetDataModel()
			? Fixture.ExistingGeneratedAnimation->GetDataModel()->GetNumberOfKeys() : -1;
		FString FailureCode;
		TestFalse(TEXT("public asset-effect failure stops the domain operation"),
			ExecuteOperation(*this, *Runtime,
				TEXT("rig.retarget.assets.generate"),
				CanonicalInput(TEXT("rig.retarget.assets.generate"), {}),
				TEXT("/Game/__UAI_RSP004/Generated"), MakeResources(),
				&FailureCode));
		TestEqual(TEXT("public asset-effect failure remains exact"),
			FailureCode, FString(FailureCase.ErrorCode));
		TestEqual(TEXT("domain never replays the failed asset effect"),
			AssetEffect->CallCount, 1);
		TestTrue(TEXT("domain performs no fallback asset mutation"),
			Fixture.ExistingGeneratedAnimation->GetSkeleton() == SkeletonBefore);
		TestEqual(TEXT("domain performs no late key mutation"),
			Fixture.ExistingGeneratedAnimation->GetDataModel()
				? Fixture.ExistingGeneratedAnimation->GetDataModel()->GetNumberOfKeys() : -1,
			KeysBefore);
	}

	AssetEffect->SetScenario(
		EScriptedAssetEffectScenario::SuccessWithUnverifiedState);
	TSharedPtr<FUeAgentRigSimulationSpatialProductionRuntime> Runtime;
	FUeAgentRigSimulationSpatialExecutionError Error;
	if (!TestTrue(TEXT("compose asset-effect readback runtime"),
		FUeAgentRigSimulationSpatialProductionRuntime::TryCreate(Config, Runtime, Error))
		|| !Runtime.IsValid()) return false;
	FVerificationOutcome Outcome;
	TestTrue(TEXT("owner success proceeds to independent asset readback"),
		ExecuteOperation(*this, *Runtime, TEXT("rig.retarget.assets.generate"),
			CanonicalInput(TEXT("rig.retarget.assets.generate"), {}),
			TEXT("/Game/__UAI_RSP004/Generated"), MakeResources(), nullptr,
			&Outcome));
	TestEqual(TEXT("independent readback rejects unverified generated state"),
		Outcome.GetStatus(), EVerificationStatus::Mismatch);
	TestEqual(TEXT("owner success alone cannot produce complete"),
		Outcome.GetEffectOutcome(), EEffectOutcome::Partial);
	TestFalse(TEXT("unverified asset effect is never retry-safe"),
		Outcome.IsRetrySafe());
	TestEqual(TEXT("successful owner invocation is still single-shot"),
		AssetEffect->CallCount, 1);
	return !HasAnyErrors();
}

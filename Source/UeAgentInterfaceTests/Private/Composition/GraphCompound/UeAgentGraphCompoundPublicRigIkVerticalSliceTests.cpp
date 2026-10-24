#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "CommandEdge/UeAgentCommandProtocol.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "PackageTools.h"
#include "Rig/IKRigDefinition.h"
#include "RigEditor/IKRigController.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::CommandRegistry;

	class FNeverCancelled final
		: public UE::UAI::Contract::IUeAgentCancellationSignal
	{
	public:
		bool IsCancelled() const override { return false; }
	};

	class FScopedPublicRigFixture final
	{
	public:
		FScopedPublicRigFixture()
			: PackageName(
				TEXT("/Game/UAI/Tests/GraphCompound/IKR_PublicSlice_")
				+ FGuid::NewGuid().ToString(EGuidFormats::Digits))
			, AssetName(FPackageName::GetLongPackageAssetName(PackageName))
		{
		}

		~FScopedPublicRigFixture()
		{
			Cleanup();
		}

		bool Create(USkeletalMesh& Mesh)
		{
			UPackage* const Package = CreatePackage(*PackageName);
			UIKRigDefinition* const Rig = NewObject<UIKRigDefinition>(
				Package,
				*AssetName,
				RF_Public | RF_Standalone);
			UIKRigController* const Controller =
				UIKRigController::GetController(Rig);
			if (!Rig || !Controller || !Controller->SetSkeletalMesh(&Mesh))
			{
				return false;
			}
			FAssetRegistryModule::AssetCreated(Rig);
			Package->MarkPackageDirty();
			FSavePackageArgs Args;
			Args.TopLevelFlags = RF_Public | RF_Standalone;
			return UPackage::SavePackage(Package, Rig, *Filename(), Args);
		}

		bool Unload() const
		{
			UPackage* const Package = FindPackage(nullptr, *PackageName);
			if (!Package)
			{
				return true;
			}
			Package->SetDirtyFlag(false);
			FText Error;
			const bool bUnloaded = UPackageTools::UnloadPackages(
				{Package}, Error, true);
			CollectGarbage(RF_NoFlags, true);
			return bUnloaded && !FindPackage(nullptr, *PackageName);
		}

		bool Cleanup()
		{
			bool bClean = true;
			if (UPackage* const Package = FindPackage(nullptr, *PackageName))
			{
				if (UObject* const Asset = FindObject<UObject>(Package, *AssetName))
				{
					FAssetRegistryModule::AssetDeleted(Asset);
				}
			}
			bClean = Unload() && bClean;
			if (IFileManager::Get().FileExists(*Filename()))
			{
				bClean = IFileManager::Get().Delete(*Filename(), false, true)
					&& bClean;
			}
			return !IFileManager::Get().FileExists(*Filename())
				&& !FindPackage(nullptr, *PackageName)
				&& bClean;
		}

		FString ObjectPath() const
		{
			return PackageName + TEXT(".") + AssetName;
		}

	private:
		FString Filename() const
		{
			return FPackageName::LongPackageNameToFilename(
				PackageName,
				FPackageName::GetAssetPackageExtension());
		}

		FString PackageName;
		FString AssetName;
	};

	FString Serialize(const TSharedRef<FJsonObject>& Object)
	{
		FString Text;
		const TSharedRef<TJsonWriter<
			TCHAR,
			TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<
				TCHAR,
				TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Object, Writer);
		return Text;
	}

	FUeAgentCommandResult SubmitPublic(
		const TSharedPtr<const FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const FUeAgentExactCommandDispatch>& Dispatch,
		const FString& CommandId,
		const TSharedRef<FJsonObject>& Params)
	{
		const auto* const Contract = Registry.IsValid()
			? Registry->FindExact(CommandId) : nullptr;
		if (!Contract || !Dispatch.IsValid())
		{
			return FUeAgentCommandResult::ProtocolError(
				TEXT("graph-public-rig-ik"),
				TEXT("graph-public-rig-ik"),
				CommandId,
				TEXT("sha256:0000000000000000000000000000000000000000000000000000000000000000"),
				TEXT("graph_public_contract_missing"),
				CommandId);
		}
		const FString RequestId = TEXT("graph-public-rig-ik-")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString Envelope = FString::Printf(
			TEXT("{\"schema\":\"uai.command.v1\",\"version\":1,")
			TEXT("\"message_kind\":\"command_request\",")
			TEXT("\"correlation_id\":\"graph-public-rig-ik\",")
			TEXT("\"request_id\":\"%s\",\"command_id\":\"%s\",")
			TEXT("\"command_binding_hash\":\"%s\",\"params\":%s}"),
			*RequestId,
			*CommandId,
			*Contract->GetContractHash(),
			*Serialize(Params));
		FTCHARToUTF8 Encoded(*Envelope);
		TArray<uint8> Bytes;
		Bytes.Append(
			reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length());
		TOptional<FUeAgentCommandRequest> Request;
		FUeAgentCommandProtocolDiagnostic Diagnostic;
		if (!FUeAgentCommandProtocolCodec().DecodeRequest(
				Bytes, Request, Diagnostic) || !Request.IsSet())
		{
			return FUeAgentCommandResult::ProtocolError(
				TEXT("graph-public-rig-ik"),
				RequestId,
				CommandId,
				Contract->GetContractHash(),
				Diagnostic.Code,
				Diagnostic.Message);
		}
		return Dispatch->Submit(
			Request.GetValue(),
			UE::UAI::Contract::FUeAgentCallerContext(
				TEXT("graph-public-rig-ik"),
				{TEXT("uai:query"), TEXT("uai:operate")},
				TEXT("graph-public-rig-ik")),
			MakeShared<FNeverCancelled>());
	}

	TSharedRef<FJsonObject> DefinitionParams(
		const FString& TargetRef,
		const FName RootBone,
		const FName EndBone)
	{
		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target_ref"), TargetRef);
		Params->SetStringField(TEXT("retarget_root"), RootBone.ToString());

		const TSharedRef<FJsonObject> Goal = MakeShared<FJsonObject>();
		Goal->SetStringField(TEXT("name"), TEXT("PublicSliceGoal"));
		Goal->SetStringField(TEXT("bone_name"), EndBone.ToString());
		Goal->SetStringField(TEXT("intent"), TEXT("create"));
		Params->SetArrayField(TEXT("goals"), {MakeShared<FJsonValueObject>(Goal)});

		const TSharedRef<FJsonObject> Solver = MakeShared<FJsonObject>();
		Solver->SetStringField(TEXT("solver_ref"), TEXT("solver:PublicSliceFBIK"));
		Solver->SetStringField(
			TEXT("solver_class_ref"),
			TEXT("/Script/IKRig.IKRigFullBodyIKSolver"));
		Solver->SetNumberField(TEXT("order_index"), 0);
		Solver->SetStringField(TEXT("intent"), TEXT("create"));
		Params->SetArrayField(
			TEXT("solvers"),
			{MakeShared<FJsonValueObject>(Solver)});

		const TSharedRef<FJsonObject> Chain = MakeShared<FJsonObject>();
		Chain->SetStringField(TEXT("name"), TEXT("PublicSliceChain"));
		Chain->SetStringField(TEXT("start_bone"), RootBone.ToString());
		Chain->SetStringField(TEXT("end_bone"), EndBone.ToString());
		Chain->SetStringField(TEXT("goal_name"), TEXT("PublicSliceGoal"));
		Chain->SetStringField(TEXT("intent"), TEXT("create"));
		Params->SetArrayField(TEXT("chains"), {MakeShared<FJsonValueObject>(Chain)});
		return Params;
	}

	bool HasOperationError(const FUeAgentCommandResult& Result, FString& OutCode)
	{
		OutCode.Reset();
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Result.GetPayloadJson());
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			OutCode = TEXT("invalid_result_json");
			return true;
		}
		const TSharedPtr<FJsonObject>* Error = nullptr;
		return Root->TryGetObjectField(TEXT("operation_error"), Error)
			&& Error && Error->IsValid()
			&& (*Error)->TryGetStringField(TEXT("code"), OutCode);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentGraphCompoundPublicRigIkVerticalSliceTest,
	"UeAgentInterface.Composition.GraphCompound.PublicRigIkVerticalSlice",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentGraphCompoundPublicRigIkVerticalSliceTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	const TStrongObjectPtr<USkeletalMesh> MeshAnchor(
		LoadObject<USkeletalMesh>(
			nullptr,
			TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube")));
	USkeletalMesh* const Mesh = MeshAnchor.Get();
	if (!TestNotNull(TEXT("owned fixture skeletal mesh exists"), Mesh)
		|| !TestTrue(
			TEXT("owned fixture exposes a non-empty bone chain"),
			Mesh && Mesh->GetRefSkeleton().GetNum() >= 2))
	{
		return false;
	}
	const FName RootBone = Mesh->GetRefSkeleton().GetBoneName(0);
	const FName EndBone = Mesh->GetRefSkeleton().GetBoneName(
		Mesh->GetRefSkeleton().GetNum() - 1);

	FScopedPublicRigFixture Fixture;
	if (!TestTrue(TEXT("owned IKRig fixture is created and saved"),
			Fixture.Create(*Mesh)))
	{
		return false;
	}

	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TestTrue(TEXT("formal public composition starts"),
			UE::UAI::Module::CommandRegistration::Active::
				TryComposeProcessActiveCommandComposition(
					Registry, Dispatch, Error))
		|| !Registry.IsValid() || !Dispatch.IsValid())
	{
		AddError(Error);
		return false;
	}

	const FUeAgentCommandResult Result = SubmitPublic(
		Registry,
		Dispatch,
		TEXT("animation.ik_rig.definition.apply"),
		DefinitionParams(Fixture.ObjectPath(), RootBone, EndBone));
	if (!TestTrue(TEXT("public command reaches the formal Graph handler"),
		Result.IsOk()))
	{
		AddError(Result.GetErrorJson());
		return false;
	}
	FString OperationError;
	if (!TestFalse(
			TEXT("public operation has no business error"),
			HasOperationError(Result, OperationError)))
	{
		AddError(OperationError);
		return false;
	}

	if (!TestTrue(TEXT("fixture unloads before independent readback"),
			Fixture.Unload()))
	{
		return false;
	}
	UIKRigDefinition* Rig = LoadObject<UIKRigDefinition>(
		nullptr,
		*Fixture.ObjectPath());
	UIKRigController* const Controller = UIKRigController::GetController(Rig);
	if (!TestNotNull(TEXT("persisted IKRig reloads"), Rig)
		|| !TestNotNull(TEXT("persisted IKRig controller reloads"), Controller))
	{
		return false;
	}
	TestEqual(TEXT("retarget root persists"),
		Controller->GetRetargetRoot(), RootBone);
	TestNotNull(TEXT("goal persists"),
		Controller->GetGoal(TEXT("PublicSliceGoal")));
	TestTrue(TEXT("solver identity and order persist"),
		Controller->GetNumSolvers() == 1
		&& Controller->GetSolverStructAtIndex(0)
		&& Controller->GetSolverStructAtIndex(0)->GetScriptStruct()
		&& Controller->GetSolverStructAtIndex(0)->GetScriptStruct()->GetPathName()
			== TEXT("/Script/IKRig.IKRigFullBodyIKSolver"));
	TestTrue(TEXT("exact chain persists"),
		Controller->GetRetargetChains().ContainsByPredicate(
			[RootBone, EndBone](const FBoneChain& Chain)
			{
				return Chain.ChainName == TEXT("PublicSliceChain")
					&& Chain.StartBone.BoneName == RootBone
					&& Chain.EndBone.BoneName == EndBone
					&& Chain.IKGoalName == TEXT("PublicSliceGoal");
			}));
	Rig = nullptr;
	TestTrue(TEXT("owned public fixture is fully released"), Fixture.Cleanup());
	return !HasAnyErrors();
}

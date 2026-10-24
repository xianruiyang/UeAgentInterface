#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "CommandEdge/UeAgentCommandProtocol.h"
#include "Components/StaticMeshComponent.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "Core/Contract/Command/UeAgentCommandLifecyclePolicy.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "OperationOwners/PhysicsDeformer/UeAgentPhysicsDeformerOperationOwner.h"
#include "OptimusDeformer.h"
#include "OptimusNode.h"
#include "OptimusNodeGraph.h"
#include "PackageTools.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"

namespace UE::UAI::Tests::StrictV2::T42
{
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandRegistry;

	class FNeverCancelled final : public Contract::IUeAgentCancellationSignal
	{
	public:
		bool IsCancelled() const override { return false; }
	};

	FUeAgentCommandResult SubmitPublic(
		const TSharedRef<const FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedRef<const FUeAgentExactCommandDispatch>& Dispatch,
		const FString& CommandId,
		const FString& ParamsJson)
	{
		const FUeAgentFrozenCommandContract* Contract =
			Registry->FindExact(CommandId);
		if (!Contract)
		{
			return FUeAgentCommandResult::ProtocolError(
				TEXT("corr-t42-public"), TEXT("req-t42-missing"), CommandId,
				TEXT("sha256:0000000000000000000000000000000000000000000000000000000000000000"),
				TEXT("t42_public_contract_missing"), CommandId);
		}
		const FString RequestId = TEXT("req-t42-")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString Envelope = FString::Printf(
			TEXT("{\"schema\":\"uai.command.v1\",\"version\":1,")
			TEXT("\"message_kind\":\"command_request\",")
			TEXT("\"correlation_id\":\"corr-t42-public\",")
			TEXT("\"request_id\":\"%s\",\"command_id\":\"%s\",")
			TEXT("\"command_binding_hash\":\"%s\",\"params\":%s}"),
			*RequestId, *CommandId, *Contract->GetContractHash(), *ParamsJson);
		const FTCHARToUTF8 Encoded(*Envelope);
		TArray<uint8> Bytes;
		Bytes.Append(
			reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length());
		TOptional<FUeAgentCommandRequest> Request;
		FUeAgentCommandProtocolDiagnostic Diagnostic;
		if (!FUeAgentCommandProtocolCodec().DecodeRequest(
				Bytes, Request, Diagnostic) || !Request.IsSet())
		{
			return FUeAgentCommandResult::ProtocolError(
				TEXT("corr-t42-public"), RequestId, CommandId,
				Contract->GetContractHash(), Diagnostic.Code, Diagnostic.Message);
		}
		return Dispatch->Submit(
			Request.GetValue(),
			Contract::FUeAgentCallerContext(
				TEXT("strict-v2-t42-public"),
				{TEXT("uai:operate")},
				TEXT("corr-t42-public")),
			MakeShared<FNeverCancelled>());
	}

	const TArray<FString>& ExpectedCommands()
	{
		static const TArray<FString> Commands = {
			TEXT("animation.deformer_graph.asset.create"),
			TEXT("animation.mesh_deformer_collection.asset.create"),
			TEXT("deformer.evaluation.sample"),
			TEXT("deformer.geometry.sample"),
			TEXT("deformer.training_inputs.validate"),
			TEXT("physics.simulation.probe"),
		};
		return Commands;
	}

	const TArray<FString>& DormantCommands()
	{
		static const TArray<FString> Commands = {
			TEXT("animation.deformer_graph.topology.apply"),
			TEXT("animation.deformer_source_library.bindings.apply"),
			TEXT("animation.mesh_deformer_collection.entries.apply"),
			TEXT("animation.ml_deformer.configuration.apply"),
			TEXT("physics.asset.structure.replace"),
			TEXT("physics.constraint.update"),
			TEXT("physics.material.update"),
		};
		return Commands;
	}

	const TArray<FString>& DisabledCommands()
	{
		static const TArray<FString> Commands = {
			TEXT("animation.deformer_source_library.asset.create"),
			TEXT("animation.ml_deformer.asset.create"),
		};
		return Commands;
	}

	bool IsQuery(const FString& CommandId)
	{
		return CommandId == TEXT("deformer.evaluation.sample")
			|| CommandId == TEXT("deformer.geometry.sample")
			|| CommandId == TEXT("deformer.training_inputs.validate");
	}

	FString ExpectedNamespace(const FString& CommandId)
	{
		int32 Separator = INDEX_NONE;
		return CommandId.FindChar(TEXT('.'), Separator)
			? CommandId.Left(Separator)
			: FString();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T42PhysicsDeformerInventoryTest,
	"UAI.StrictV2.T42.PHYSICS.DEFORMER.OPERATION.InventoryAndActivation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T42PhysicsDeformerInventoryTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	using namespace UE::UAI::Tests::StrictV2::T42;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TestTrue(TEXT("the process active composition freezes"),
		TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error))
		|| !Registry.IsValid())
	{
		AddError(Error);
		return false;
	}
	for (const FString& CommandId : ExpectedCommands())
	{
		const auto* Contract = Registry->FindExact(CommandId);
		TestNotNull(*FString::Printf(TEXT("%s is active"), *CommandId), Contract);
		if (Contract)
		{
			TestEqual(*FString::Printf(TEXT("%s is enabled"), *CommandId),
				Contract->GetAvailability(),
				CommandContract::EUeAgentCommandAvailability::Enabled);
			TestEqual(*FString::Printf(TEXT("%s has one T42 owner"), *CommandId),
				Contract->GetPublicOwner(),
				FString(TEXT("physics-deformer-operation-owner")));
			const FString Namespace = ExpectedNamespace(CommandId);
			TestEqual(*FString::Printf(TEXT("%s uses its stable namespace"),
				*CommandId), Contract->GetPythonNamespace(), Namespace);
			TestEqual(*FString::Printf(TEXT("%s uses its stable method"),
				*CommandId), Contract->GetPythonFunction(),
				CommandId.RightChop(Namespace.Len() + 1)
					.Replace(TEXT("."), TEXT("_")));
		}
	}
	for (const FString& CommandId : DormantCommands())
	{
		const auto* Contract = Registry->FindExact(CommandId);
		TestNotNull(*FString::Printf(TEXT("%s remains registered"), *CommandId), Contract);
		if (Contract)
		{
			TestEqual(
				*FString::Printf(TEXT("%s is centrally dormant"), *CommandId),
				Contract->GetAvailability(),
				CommandContract::EUeAgentCommandAvailability::Dormant);
		}
	}
	for (const FString& CommandId : DisabledCommands())
	{
		const auto* Contract = Registry->FindExact(CommandId);
		TestNotNull(*FString::Printf(TEXT("%s remains registered"), *CommandId),
			Contract);
		if (Contract)
		{
			TestEqual(*FString::Printf(TEXT("%s is disabled"), *CommandId),
				Contract->GetAvailability(),
				CommandContract::EUeAgentCommandAvailability::Disabled);
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T42PhysicsDeformerDomainCapabilityTest,
	"UAI.StrictV2.T42.PHYSICS.DEFORMER.OPERATION.DomainCapabilityAndOperationFactories",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T42PhysicsDeformerDomainCapabilityTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::DomainCapability;
	using namespace UE::UAI::Tests::StrictV2::T42;
	(void)Parameters;
	TSharedPtr<const FFrozenDomainCapabilitySet> Frozen;
	FString Error;
	if (!TestTrue(TEXT("the process domain capabilities freeze"),
		TryFreezeProcessDomainCapabilities(Frozen, Error))
		|| !Frozen.IsValid())
	{
		AddError(Error);
		return false;
	}
	const TSharedRef<const FDomainCapabilityContribution>* Domain =
		Frozen->GetContributions().FindByPredicate(
			[](const TSharedRef<const FDomainCapabilityContribution>& Item)
			{
				return Item->GetDomainId() == TEXT("physics_deformer");
			});
	if (!TestNotNull(TEXT("Physics/Deformer owns one typed contribution"), Domain))
	{
		return false;
	}
	const FDomainCapabilityDeclaration& Declaration =
		(*Domain)->GetDeclaration();
	TestEqual(TEXT("Physics/Deformer owns one command contribution"),
		Declaration.Command.Contributions.Num(), 1);
	for (const FString& CommandId : ExpectedCommands())
	{
		if (IsQuery(CommandId))
		{
			continue;
		}
		const TSharedPtr<const IDomainOperationInvocationFactory> Factory =
			Frozen->FindOperationFactory(CommandId);
		const TSharedPtr<const IDomainOperationInvocationFactory> PublicFactory =
			UE::UAI::OperationOwners::PhysicsDeformer::
				FindOperationInvocationFactory(CommandId);
		TestTrue(*FString::Printf(TEXT("%s has one request factory"), *CommandId),
			Factory.IsValid());
		TestTrue(*FString::Printf(
			TEXT("%s public route shares the typed factory"), *CommandId),
			Factory.IsValid() && PublicFactory.IsValid()
				&& Factory.Get() == PublicFactory.Get());
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T42PhysicsDeformerPublicProductTest,
	"UAI.StrictV2.T42.PHYSICS.DEFORMER.OPERATION.PublicDispatchToRealDeformer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T42PhysicsDeformerPublicProductTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::Tests::StrictV2::T42;
	(void)Parameters;
	const FString PackageName = TEXT("/Game/UAI_Automation/T42/PublicDeformer_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	const FString ObjectPath = PackageName + TEXT(".") + AssetName;
	const FString Filename = FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetAssetPackageExtension());
	USkeletalMesh* const Mesh = LoadObject<USkeletalMesh>(
		nullptr, TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
	if (!TestNotNull(TEXT("the public fixture has a real skeletal mesh"), Mesh))
	{
		return false;
	}

	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	const bool bComposed = Module::CommandRegistration::Active::
		TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error);
	if (!TestTrue(TEXT("the public product registry composes"), bComposed)
		|| !Registry.IsValid() || !Dispatch.IsValid())
	{
		AddError(Error);
		return false;
	}

	const FString CreateParams = FString::Printf(
		TEXT("{\"asset_name\":\"%s\",\"package_path\":\"%s\",")
		TEXT("\"skeletal_mesh_ref\":\"%s\"}"),
		*AssetName, *FPackageName::GetLongPackagePath(PackageName),
		*Mesh->GetPathName());
	const CommandEdge::FUeAgentCommandResult CreateResult = SubmitPublic(
		Registry.ToSharedRef(), Dispatch.ToSharedRef(),
		TEXT("animation.deformer_graph.asset.create"), CreateParams);
	if (!CreateResult.IsOk())
	{
		AddInfo(TEXT("public_create_error=") + CreateResult.GetErrorJson());
		AddInfo(TEXT("public_create_payload=") + CreateResult.GetPayloadJson());
	}
	TestTrue(TEXT("public create reaches the real Deformer owner"),
		CreateResult.IsOk());
	TestTrue(TEXT("public create saves the real package"),
		IFileManager::Get().FileExists(*Filename));
	UOptimusDeformer* Deformer = LoadObject<UOptimusDeformer>(nullptr, *ObjectPath);
	TestTrue(TEXT("public create independently reads the requested mesh"),
		Deformer && Deformer->Mesh.Get() == Mesh);

	const FString TopologyParams = FString::Printf(
		TEXT("{\"target_ref\":\"%s\",\"edits\":[{")
		TEXT("\"identity\":\"item:T42PublicComment\",")
		TEXT("\"element_kind\":\"item\",\"intent\":\"create\",")
		TEXT("\"value\":{\"node_name\":\"Comment_T42_Public\"}}]}"),
		*ObjectPath);
	const CommandEdge::FUeAgentCommandResult TopologyResult = SubmitPublic(
		Registry.ToSharedRef(), Dispatch.ToSharedRef(),
		TEXT("animation.deformer_graph.topology.apply"), TopologyParams);
	if (!TopologyResult.IsOk())
	{
		AddInfo(TEXT("public_topology_error=") + TopologyResult.GetErrorJson());
		AddInfo(TEXT("public_topology_payload=") + TopologyResult.GetPayloadJson());
	}
	TestTrue(TEXT("public topology reaches the same real Deformer owner"),
		TopologyResult.IsOk());
	Deformer = LoadObject<UOptimusDeformer>(nullptr, *ObjectPath);
	TestTrue(TEXT("public topology independently reads the native comment node"),
		Deformer && Deformer->GetUpdateGraph()
			&& Deformer->GetUpdateGraph()->GetAllNodes().ContainsByPredicate(
				[](const UOptimusNode* Node)
				{
					return Node && Node->GetFName() == TEXT("Comment_T42_Public");
				}));

	if (Deformer)
	{
		FAssetRegistryModule::AssetDeleted(Deformer);
	}
	if (UPackage* const Package = FindPackage(nullptr, *PackageName))
	{
		Package->SetDirtyFlag(false);
		FText UnloadError;
		UPackageTools::UnloadPackages({Package}, UnloadError, true);
	}
	CollectGarbage(RF_NoFlags, true);
	if (IFileManager::Get().FileExists(*Filename))
	{
		IFileManager::Get().Delete(*Filename, false, true);
	}
	TestFalse(TEXT("the public Deformer fixture leaves no package file"),
		IFileManager::Get().FileExists(*Filename));
	TestNull(TEXT("the public Deformer fixture leaves no loaded package"),
		FindPackage(nullptr, *PackageName));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T42PhysicsPublicSessionTest,
	"UAI.StrictV2.T42.PHYSICS.DEFORMER.OPERATION.PublicPhysicsSession",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T42PhysicsPublicSessionTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::Tests::StrictV2::T42;
	(void)Parameters;
	UStaticMesh* const Cube = LoadObject<UStaticMesh>(
		nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	const FName SubjectName(*(TEXT("T42PublicPhysicsSubject_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	UStaticMeshComponent* const Subject = NewObject<UStaticMeshComponent>(
		GetTransientPackage(), SubjectName, RF_Transient);
	if (!TestNotNull(TEXT("the public Physics fixture has a real mesh"), Cube)
		|| !TestNotNull(TEXT("the public Physics fixture has a subject"), Subject))
	{
		return false;
	}
	Subject->SetStaticMesh(Cube);
	Subject->SetMobility(EComponentMobility::Movable);
	Subject->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Subject->SetWorldTransform(FTransform(FVector(0.0, 0.0, 200.0)));
	const FTransform OriginalTransform = Subject->GetComponentTransform();

	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	const bool bComposed = Module::CommandRegistration::Active::
		TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error);
	if (!TestTrue(TEXT("the public Physics registry composes"), bComposed)
		|| !Registry.IsValid() || !Dispatch.IsValid())
	{
		AddError(Error);
		return false;
	}

	const FString SubjectRef = Subject->GetPathName();
	const FString SessionId = TEXT("t42-public-session-")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Input = FString::Printf(
		TEXT("{\"subject_refs\":[\"%s\"],\"simulation_session\":{")
		TEXT("\"session_id\":\"%s\",\"owner_ref\":\"%s\",")
		TEXT("\"generation\":\"1\"},\"simulation_range\":{")
		TEXT("\"clock_id\":\"ue.physics.seconds\",\"lower_tick\":\"0\",")
		TEXT("\"upper_tick\":\"1\",\"rate\":{\"numerator\":\"60\",")
		TEXT("\"denominator\":\"1\"},\"state\":\"simulation\"},")
		TEXT("\"impulses\":[{\"subject_ref\":\"%s\",\"time\":{")
		TEXT("\"clock_id\":\"ue.physics.seconds\",\"tick\":\"0\",")
		TEXT("\"rate\":{\"numerator\":\"60\",\"denominator\":\"1\"},")
		TEXT("\"state\":\"simulation\"},\"frame\":{")
		TEXT("\"frame_id\":\"ue.physics.world\",\"basis_frame_id\":\"ue.world\",")
		TEXT("\"dimension\":3,\"unit\":\"cm\",")
		TEXT("\"basis_evidence_id\":\"uai.physics.world.identity.v1\"},")
		TEXT("\"components\":[\"120\",\"0\",\"0\"],")
		TEXT("\"evidence_ids\":[\"t42-public-impulse\"]}],")
		TEXT("\"velocities\":[{\"subject_ref\":\"%s\",\"time\":{")
		TEXT("\"clock_id\":\"ue.physics.seconds\",\"tick\":\"0\",")
		TEXT("\"rate\":{\"numerator\":\"60\",\"denominator\":\"1\"},")
		TEXT("\"state\":\"simulation\"},\"frame\":{")
		TEXT("\"frame_id\":\"ue.physics.world\",\"basis_frame_id\":\"ue.world\",")
		TEXT("\"dimension\":3,\"unit\":\"cm\",")
		TEXT("\"basis_evidence_id\":\"uai.physics.world.identity.v1\"},")
		TEXT("\"components\":[\"0\",\"60\",\"0\"],")
		TEXT("\"evidence_ids\":[\"t42-public-velocity\"]}]}"),
		*SubjectRef, *SessionId, *SubjectRef, *SubjectRef, *SubjectRef);
	const FString Params = FString::Printf(
		TEXT("{\"target\":\"%s\",\"input\":%s,\"context\":{}}"),
		*SubjectRef, *Input);
	const CommandEdge::FUeAgentCommandResult Result = SubmitPublic(
		Registry.ToSharedRef(), Dispatch.ToSharedRef(),
		TEXT("physics.simulation.probe"), Params);
	if (!Result.IsOk())
	{
		AddInfo(TEXT("public_physics_error=") + Result.GetErrorJson());
		AddInfo(TEXT("public_physics_payload=") + Result.GetPayloadJson());
	}
	TestTrue(TEXT("public probe reaches the production session owner"),
		Result.IsOk());
	TestTrue(TEXT("public probe returns completed independent observations"),
		Result.GetPayloadJson().Contains(TEXT("\"effect_outcome\":\"complete\""))
			&& Result.GetPayloadJson().Contains(TEXT("\"status\":\"verified\""))
			&& Result.GetPayloadJson().Contains(
				TEXT("independent-state:rig-simulation-spatial:physics.simulation.probe")));
	TestTrue(TEXT("the production session never mutates the source component"),
		Subject->GetComponentTransform().Equals(OriginalTransform, 0.001));
	Subject->MarkAsGarbage();
	CollectGarbage(RF_NoFlags, true);
	return !HasAnyErrors();
}

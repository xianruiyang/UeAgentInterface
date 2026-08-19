#include "Misc/AutomationTest.h"

#include "CommandEdge/UeAgentCommandProtocol.h"
#include "Composition/SimpleAuthoring/OperationHost/UeAgentSimpleAuthoringOperationHost.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "Landscape.h"
#include "LandscapeDataAccess.h"
#include "LandscapeEdit.h"
#include "LandscapeInfo.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "OperationOwners/Landscape/UeAgentLandscapeOperationOwner.h"
#include "PackageTools.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"

namespace UE::UAI::Tests::StrictV2::T41
{
	using namespace CommandEdge;
	using namespace CommandRegistry;
	using namespace CommandContract;

	class FNeverCancelled final : public Contract::IUeAgentCancellationSignal
	{
	public:
		bool IsCancelled() const override { return false; }
	};

	class FPersistedUnverifiedLandscapeHandler final
		: public IUeAgentCommandHandler
	{
	public:
		const FString& GetCommandId() const override
		{
			static const FString CommandId(TEXT("landscape.create"));
			return CommandId;
		}

		FUeAgentCommandResult Submit(
			const FUeAgentCommandRequest& Request,
			const Contract::FUeAgentCallerContext&,
			const TSharedRef<Contract::IUeAgentCancellationSignal>&) override
		{
			return FUeAgentCommandResult::Failure(
				Request,
				TEXT("{}"),
				TEXT("{\"code\":\"verification_oracle_unavailable\","
					"\"detail\":\"persisted_unverified\"}"));
		}
	};

	const TArray<FString>& ExpectedCommands()
	{
		static const TArray<FString> Commands = {
			TEXT("landscape.create"),
			TEXT("landscape.height_delta"),
		};
		return Commands;
	}

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
				TEXT("corr-t41-public"), TEXT("req-t41-missing"), CommandId,
				TEXT("sha256:0000000000000000000000000000000000000000000000000000000000000000"),
				TEXT("t41_public_contract_missing"), CommandId);
		}
		const FString RequestId = TEXT("req-t41-")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString Envelope = FString::Printf(
			TEXT("{\"schema\":\"uai.command.v1\",\"version\":1,")
			TEXT("\"message_kind\":\"command_request\",")
			TEXT("\"correlation_id\":\"corr-t41-public\",")
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
				TEXT("corr-t41-public"), RequestId, CommandId,
				Contract->GetContractHash(), Diagnostic.Code, Diagnostic.Message);
		}
		return Dispatch->Submit(
			Request.GetValue(),
			Contract::FUeAgentCallerContext(
				TEXT("strict-v2-t41-public"),
				{TEXT("uai:operate")},
				TEXT("corr-t41-public")),
			MakeShared<FNeverCancelled>());
	}

	ALandscape* FindLandscape(UWorld& World, const FString& Label)
	{
		ALandscape* Match = nullptr;
		for (TActorIterator<ALandscape> It(&World); It; ++It)
		{
			if (It->GetActorLabel() == Label)
			{
				if (Match)
				{
					return nullptr;
				}
				Match = *It;
			}
		}
		return Match;
	}

	bool ReadHeight(ALandscape& Landscape, const int32 X, const int32 Y,
		float& OutHeight)
	{
		ULandscapeInfo* Info = Landscape.GetLandscapeInfo();
		if (!Info)
		{
			return false;
		}
		uint16 Encoded = 0;
		FLandscapeEditDataInterface Edit(Info);
		Edit.GetHeightDataFast(X, Y, X, Y, &Encoded, 1, nullptr);
		OutHeight = LandscapeDataAccess::GetLocalHeight(Encoded);
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T41LandscapeInventoryTest,
	"UAI.StrictV2.T41.LANDSCAPE.OPERATION.InventoryAndActivation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T41LandscapeInventoryTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	using namespace UE::UAI::Tests::StrictV2::T41;
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
	TestEqual(TEXT("T41 owns exactly 2 inventory commands"),
		ExpectedCommands().Num(), 2);
	for (const FString& CommandId : ExpectedCommands())
	{
		const auto* Contract = Registry->FindExact(CommandId);
		TestNotNull(*FString::Printf(TEXT("%s is active"), *CommandId), Contract);
		if (Contract)
		{
			TestEqual(*FString::Printf(TEXT("%s has one T41 owner"), *CommandId),
				Contract->GetPublicOwner(),
				FString(TEXT("landscape-operation-owner")));
			TestEqual(*FString::Printf(TEXT("%s uses the stable namespace"), *CommandId),
				Contract->GetPythonNamespace(), FString(TEXT("landscape")));
			TestEqual(*FString::Printf(TEXT("%s uses the stable method"), *CommandId),
				Contract->GetPythonFunction(),
				CommandId.RightChop(FString(TEXT("landscape.")).Len())
					.Replace(TEXT("."), TEXT("_")));
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T41LandscapeDomainCapabilityTest,
	"UAI.StrictV2.T41.LANDSCAPE.OPERATION.DomainCapabilityAndOperationFactories",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T41LandscapeDomainCapabilityTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::DomainCapability;
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
	const TSharedRef<const FDomainCapabilityContribution>* Landscape =
		Frozen->GetContributions().FindByPredicate(
			[](const TSharedRef<const FDomainCapabilityContribution>& Item)
			{
				return Item->GetDomainId() == TEXT("landscape");
			});
	if (!TestNotNull(TEXT("Landscape owns one typed contribution"), Landscape))
	{
		return false;
	}
	const FDomainCapabilityDeclaration& Declaration =
		(*Landscape)->GetDeclaration();
	TestEqual(TEXT("Landscape owns one command contribution"),
		Declaration.Command.Contributions.Num(), 1);
	TestEqual(TEXT("Landscape owns both write operations"),
		Declaration.Operation.Operations.Num(), 2);
	TestEqual(TEXT("Landscape migration owner is exact"),
		Declaration.Migration.OwnerId,
		FString(TEXT("T41-LANDSCAPE-OPERATION")));
	for (const FString& CommandId :
		UE::UAI::Tests::StrictV2::T41::ExpectedCommands())
	{
		const TSharedPtr<const IDomainOperationInvocationFactory> Factory =
			Frozen->FindOperationFactory(CommandId);
		const TSharedPtr<const IDomainOperationInvocationFactory> PublicFactory =
			UE::UAI::OperationOwners::Landscape::
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
	FUeAgentStrictV2T41LandscapePublicProductTest,
	"UAI.StrictV2.T41.LANDSCAPE.OPERATION.PublicDispatchToRealLandscape",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T41LandscapePublicProductTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::Tests::StrictV2::T41;
	(void)Parameters;
	const FString PackageName = TEXT("/Game/UAI_Automation/T41/Public_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Filename = FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetMapPackageExtension());
	UPackage* Package = CreatePackage(*PackageName);
	UWorld* World = UWorld::CreateWorld(
		EWorldType::EditorPreview,
		false,
		FName(*FPackageName::GetLongPackageAssetName(PackageName)),
		Package);
	if (!TestTrue(TEXT("the public fixture owns one real Editor world"),
			Package && World && World->PersistentLevel))
	{
		return false;
	}
	World->SetFlags(RF_Public | RF_Standalone);
	World->PersistentLevel->SetUseExternalActors(false);
	World->SetCurrentLevel(World->PersistentLevel);
	FWorldContext& OwnedWorldContext =
		GEngine->CreateNewWorldContext(EWorldType::EditorPreview);
	OwnedWorldContext.SetCurrentWorld(World);
	Composition::SimpleAuthoring::OperationHost::
		SetAutomationWorldOverride(World);

	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	const bool bComposed = Module::CommandRegistration::Active::
		TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error);
	if (!TestTrue(TEXT("the public product registry composes"), bComposed)
		|| !Registry.IsValid() || !Dispatch.IsValid())
	{
		AddError(Error);
		Composition::SimpleAuthoring::OperationHost::
			SetAutomationWorldOverride(nullptr);
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
		World->MarkAsGarbage();
		Package->MarkAsGarbage();
		return false;
	}

	const FString TargetRef = TEXT("uai://landscape/t41-public-created");
	const FString CreateParams = FString::Printf(
		TEXT("{\"initial_height\":{\"encoding\":\"landscape_local_height\","
			"\"value\":0},\"placement\":{\"basis_evidence\":\"owned public "
			"world identity transform\",\"frame_id\":\"ue.world\","
			"\"rotation_quaternion_xyzw\":[0,0,0,1],\"scale\":[100,100,50],"
			"\"subject_point\":\"sample_origin\",\"translation\":[0,0,0],"
			"\"unit\":\"centimeter\"},\"sample_region\":{"
			"\"basis_evidence\":\"one component one section seven quads\","
			"\"frame_id\":\"ue.landscape.local_sample_grid\","
			"\"max_inclusive\":[7,7],\"min_inclusive\":[0,0]},"
			"\"target_ref\":\"%s\",\"topology\":{"
			"\"component_count\":[1,1],\"quads_per_section\":7,"
			"\"sections_per_component\":1}}"),
		*TargetRef);
	const CommandEdge::FUeAgentCommandResult CreateResult = SubmitPublic(
		Registry.ToSharedRef(), Dispatch.ToSharedRef(),
		TEXT("landscape.create"), CreateParams);
	if (!CreateResult.IsOk())
	{
		AddInfo(TEXT("public_create_error=") + CreateResult.GetErrorJson());
		AddInfo(TEXT("public_create_payload=") + CreateResult.GetPayloadJson());
	}
	TestTrue(TEXT("public landscape.create reaches the real operation owner"),
		CreateResult.IsOk());
	ALandscape* Landscape = FindLandscape(
		*World, TEXT("t41-public-created"));
	TestNotNull(TEXT("public create produced one real ALandscape"), Landscape);
	TestTrue(TEXT("public create persisted the owned world package"),
		IFileManager::Get().FileExists(*Filename));

	float Before = 0.0f;
	if (Landscape)
	{
		TestTrue(TEXT("public delta pre-state is read through Landscape API"),
			ReadHeight(*Landscape, 4, 4, Before));
	}
	const FString DeltaTargetRef = Landscape
		? Landscape->GetPathName() : FString(TEXT("missing"));
	const FString DeltaParams = FString::Printf(
		TEXT("{\"delta\":{\"encoding\":\"landscape_local_height_delta\","
			"\"row_stride\":1,\"values\":[6]},\"region\":{"
			"\"frame_id\":\"ue.landscape.local_sample_grid\","
			"\"max_inclusive\":[4,4],\"min_inclusive\":[4,4]},"
			"\"space_contract\":{\"basis_evidence\":\"public fixture live "
			"transform extent and revision\",\"conversion_chain\":\"world "
			"centimeters to local sample grid and local height\","
			"\"expected_scale\":[100,100,50],"
			"\"read_frame_id\":\"ue.landscape.local_sample_grid\","
			"\"semantic_frame_id\":\"ue.world\",\"unit\":\"centimeter\","
			"\"verify_frame_id\":\"ue.world\","
			"\"write_frame_id\":\"ue.landscape.local_sample_grid\"},"
			"\"target_ref\":\"%s\"}"),
		*DeltaTargetRef);
	const CommandEdge::FUeAgentCommandResult DeltaResult = SubmitPublic(
		Registry.ToSharedRef(), Dispatch.ToSharedRef(),
		TEXT("landscape.height_delta"), DeltaParams);
	if (!DeltaResult.IsOk())
	{
		AddInfo(TEXT("public_delta_error=") + DeltaResult.GetErrorJson());
		AddInfo(TEXT("public_delta_payload=") + DeltaResult.GetPayloadJson());
	}
	TestTrue(TEXT("public height_delta reaches the same real operation path"),
		DeltaResult.IsOk());
	float After = 0.0f;
	if (Landscape)
	{
		TestTrue(TEXT("public height_delta has independent native readback"),
			ReadHeight(*Landscape, 4, 4, After));
		TestTrue(TEXT("public height_delta preserves the requested world height"),
			FMath::IsNearlyEqual((After - Before) * 50.0f, 300.0f, 0.5f));
	}

	const FString MissingParams = DeltaParams.Replace(
		*DeltaTargetRef,
		*(World->PersistentLevel->GetPathName() + TEXT(".MissingLandscape")));
	const CommandEdge::FUeAgentCommandResult MissingResult = SubmitPublic(
		Registry.ToSharedRef(), Dispatch.ToSharedRef(),
		TEXT("landscape.height_delta"), MissingParams);
	TestFalse(TEXT("public missing target is a typed prewrite failure"),
		MissingResult.IsOk());
	TestTrue(TEXT("public prewrite failure is rejected and retry-safe"),
		MissingResult.GetErrorJson().Contains(
			TEXT("\"terminal_status\":\"rejected\""))
			&& MissingResult.GetErrorJson().Contains(
				TEXT("\"retry_safe\":true")));

	Composition::SimpleAuthoring::OperationHost::
		SetAutomationHandlerOverride(
			TEXT("landscape.create"),
			MakeShared<FPersistedUnverifiedLandscapeHandler>());
	const CommandEdge::FUeAgentCommandResult UnverifiableResult = SubmitPublic(
		Registry.ToSharedRef(), Dispatch.ToSharedRef(),
		TEXT("landscape.create"), CreateParams);
	Composition::SimpleAuthoring::OperationHost::
		SetAutomationHandlerOverride(TEXT("landscape.create"), nullptr);
	TestFalse(TEXT("a post-persist verification failure is not success"),
		UnverifiableResult.IsOk());
	TestTrue(
		TEXT("a delegated post-mutation failure remains state_unknown and unsafe to replay"),
		UnverifiableResult.GetErrorJson().Contains(
			TEXT("\"terminal_status\":\"state_unknown\""))
			&& UnverifiableResult.GetErrorJson().Contains(
				TEXT("\"retry_safe\":false")));

	Composition::SimpleAuthoring::OperationHost::
		SetAutomationWorldOverride(nullptr);
	GEngine->DestroyWorldContext(World);
	Package->SetDirtyFlag(false);
	World->DestroyWorld(false);
	World->MarkAsGarbage();
	Package->MarkAsGarbage();
	if (IFileManager::Get().FileExists(*Filename))
	{
		IFileManager::Get().Delete(*Filename, false, true);
	}
	FText UnloadError;
	UPackageTools::UnloadPackages({Package}, UnloadError, true);
	CollectGarbage(RF_NoFlags, true);
	if (IFileManager::Get().FileExists(*Filename))
	{
		IFileManager::Get().Delete(*Filename, false, true);
	}
	TestFalse(TEXT("the public product fixture leaves no package file"),
		IFileManager::Get().FileExists(*Filename));
	TestNull(TEXT("the public product fixture leaves no loaded package"),
		FindPackage(nullptr, *PackageName));
	return !HasAnyErrors();
}

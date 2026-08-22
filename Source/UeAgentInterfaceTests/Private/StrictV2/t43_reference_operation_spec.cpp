#include "Misc/AutomationTest.h"

#include "CommandEdge/UeAgentCommandProtocol.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "DomainAdapters/ReferenceAsset/UeAgentReferenceAssetAdapter.h"
#include "DomainConformance/UeAgentDomainConformanceHarness.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "OperationOwners/Reference/UeAgentReferenceOperationOwner.h"

namespace UE::UAI::Tests::StrictV2::T43
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
		const TSharedPtr<const FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const FUeAgentExactCommandDispatch>& Dispatch,
		const FString& CommandId,
		const FString& ParamsJson)
	{
		const FUeAgentFrozenCommandContract* Contract = Registry.IsValid()
			? Registry->FindExact(CommandId) : nullptr;
		if (!Contract || !Dispatch.IsValid())
		{
			return FUeAgentCommandResult::ProtocolError(
				TEXT("corr-t43-product"), TEXT("req-t43-product"), CommandId,
				TEXT("sha256:0000000000000000000000000000000000000000000000000000000000000000"),
				TEXT("t43_public_contract_missing"), CommandId);
		}
		const FString RequestId = TEXT("t43-product-")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString Envelope = FString::Printf(
			TEXT("{\"schema\":\"uai.command.v1\",\"version\":1,")
			TEXT("\"message_kind\":\"command_request\",")
			TEXT("\"correlation_id\":\"corr-t43-product\",")
			TEXT("\"request_id\":\"%s\",\"command_id\":\"%s\",")
			TEXT("\"command_binding_hash\":\"%s\",\"params\":%s}"),
			*RequestId, *CommandId, *Contract->GetContractHash(), *ParamsJson);
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
				TEXT("corr-t43-product"), RequestId, CommandId,
				Contract->GetContractHash(), Diagnostic.Code, Diagnostic.Message);
		}
		return Dispatch->Submit(
			Request.GetValue(),
			Contract::FUeAgentCallerContext(
				TEXT("strict-v2-t43-product"),
				{TEXT("uai:query"), TEXT("uai:operate")},
				TEXT("corr-t43-product")),
			MakeShared<FNeverCancelled>());
	}

	FString Params(
		const FString& ObjectPath,
		const FString& Intent,
		const FString& ProposedText = FString())
	{
		FString Result = FString::Printf(
			TEXT("{\"object_path\":\"%s\",\"intent\":\"%s\""),
			*ObjectPath, *Intent);
		if (!ProposedText.IsEmpty())
		{
			Result += FString::Printf(
				TEXT(",\"proposed_text\":\"%s\""), *ProposedText);
		}
		return Result + TEXT("}");
	}

	FString PackageFilename(const FString& ObjectPath)
	{
		FString PackageName;
		FString NormalizedObjectPath;
		FString ErrorCode;
		if (!DomainAdapters::ReferenceAsset::TryNormalizeReferenceAssetPath(
				ObjectPath, PackageName, NormalizedObjectPath, ErrorCode))
		{
			return FString();
		}
		FString Filename;
		FPackageName::TryConvertLongPackageNameToFilename(
			PackageName, Filename, FPackageName::GetAssetPackageExtension());
		return FPaths::ConvertRelativePathToFull(Filename);
	}

	void CleanupOwnedFixture(const FString& ObjectPath)
	{
		const FString Filename = PackageFilename(ObjectPath);
		if (Filename.IsEmpty())
		{
			return;
		}
		IFileManager::Get().Delete(*Filename, false, true, true);
		IFileManager::Get().Delete(
			*FPaths::ChangeExtension(Filename, TEXT("uexp")), false, true, true);
		IFileManager::Get().Delete(
			*FPaths::ChangeExtension(Filename, TEXT("ubulk")), false, true, true);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T43ReferenceOperationInventoryTest,
	"UAI.StrictV2.T43.REFERENCE.OPERATION.InventoryAndActivation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T43ReferenceOperationInventoryTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::DomainCapability;
	using namespace UE::UAI::DomainConformance;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	using namespace UE::UAI::Tests::StrictV2::T43;
	(void)Parameters;
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
	const auto* Contract = Registry->FindExact(TEXT("reference.asset_apply"));
	TestNotNull(TEXT("reference.asset_apply is active"), Contract);
	if (Contract)
	{
		TestEqual(TEXT("reference.asset_apply has one T43 owner"),
			Contract->GetPublicOwner(),
			FString(TEXT("reference-operation-owner")));
		TestEqual(TEXT("reference.asset_apply uses the stable SDK namespace"),
			Contract->GetPythonNamespace(), FString(TEXT("reference")));
		TestEqual(TEXT("reference.asset_apply uses the stable SDK method"),
			Contract->GetPythonFunction(), FString(TEXT("asset_apply")));
	}

	TSharedPtr<const FFrozenDomainCapabilitySet> FrozenCapabilities;
	if (!TestTrue(TEXT("the process domain capability set freezes"),
		TryFreezeProcessDomainCapabilities(FrozenCapabilities, Error))
		|| !FrozenCapabilities.IsValid())
	{
		AddError(Error);
		return false;
	}
	const TSharedRef<const FDomainCapabilityContribution>* Reference =
		FrozenCapabilities->GetContributions().FindByPredicate(
			[](const TSharedRef<const FDomainCapabilityContribution>& Item)
			{
				return Item->GetDomainId() == TEXT("reference-operation");
			});
	TestNotNull(TEXT("T43 owns one typed domain contribution"), Reference);
	if (!Reference)
	{
		return false;
	}
	const FDomainCapabilityDeclaration& Declaration =
		(*Reference)->GetDeclaration();
	TestEqual(TEXT("T43 contributes one public command owner"),
		Declaration.Command.Contributions.Num(), 1);
	TestEqual(TEXT("T43 declares one durable operation"),
		Declaration.Operation.Operations.Num(), 1);
	if (Declaration.Operation.Operations.Num() == 1)
	{
		const FDomainOperationEntry& Operation =
			Declaration.Operation.Operations[0];
		TestEqual(TEXT("the operation identity is exact"),
			Operation.OperationId, FString(TEXT("reference.asset_apply")));
		TestTrue(TEXT("the operation persists"), Operation.bPersists);
		TestTrue(TEXT("the operation owns independent readback"),
			Operation.bIndependentReadback);
		TestTrue(TEXT("the operation factory is request scoped"),
			Operation.InvocationFactory.IsValid());
		TestTrue(TEXT("the handler and contribution share one factory"),
			Operation.InvocationFactory.IsValid()
				&& &OperationOwners::Reference::FindOperationInvocationFactory().Get()
					== Operation.InvocationFactory.Get());
	}
	TestEqual(TEXT("T43 owns one migration identity"),
		Declaration.Migration.OwnerId,
		FString(TEXT("T43-REFERENCE-OPERATION")));

	FDomainConformanceFixture Fixture;
	Fixture.FixtureId = TEXT("t43-reference-operation");
	Fixture.DomainId = TEXT("reference-operation");
	Fixture.TargetIdentity = TEXT("/Game/UAI/Reference/T43_Conformance.T43_Conformance");
	Fixture.BaseRevision = TEXT("reference-contract-current");
	Fixture.MechanicsFamily = TEXT("reference-asset-crud");
	Fixture.PublicRouteId = TEXT("reference.asset_apply");
	Fixture.PublicAutomationTestId =
		TEXT("UAI.StrictV2.T43.REFERENCE.OPERATION.PublicProductLifecycle");
	Fixture.bPublicRoute = true;
	Fixture.bRealMechanics = true;
	Fixture.bIndependentReadback = true;
	Fixture.bPersistReload = true;
	Fixture.bFailureCleanup = true;
	Fixture.bLegacyExit = true;
	FDomainConformancePlan Plan;
	TestTrue(TEXT("T43 consumes the shared conformance planner"),
		TryBuildDomainConformancePlan(*Reference, Fixture, Plan, Error));
	TestTrue(TEXT("T43 plans process authority adoption"),
		Plan.Contains(EDomainConformanceScenario::ProcessAuthorityAdoption));
	TestTrue(TEXT("T43 plans request-factory isolation"),
		Plan.Contains(EDomainConformanceScenario::OperationRequestFactory));
	TestTrue(TEXT("T43 plans durable independent readback"),
		Plan.Contains(EDomainConformanceScenario::PersistReloadReadback));
	TestTrue(TEXT("T43 plans failure cleanup"),
		Plan.Contains(EDomainConformanceScenario::FailureCleanup));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T43ReferenceOperationPublicProductTest,
	"UAI.StrictV2.T43.REFERENCE.OPERATION.PublicProductLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T43ReferenceOperationPublicProductTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	using namespace UE::UAI::Tests::StrictV2::T43;
	(void)Parameters;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error))
	{
		AddError(Error);
		return false;
	}
	const FString FixtureName = TEXT("T43_Product_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString ObjectPath = TEXT("/Game/UAI/Reference/") + FixtureName
		+ TEXT(".") + FixtureName;
	const FString Filename = PackageFilename(ObjectPath);
	CleanupOwnedFixture(ObjectPath);

	auto RequireSuccess = [this](
		const TCHAR* Stage,
		const CommandEdge::FUeAgentCommandResult& Result)
	{
		if (!Result.IsOk())
		{
			AddError(FString(Stage) + TEXT(": ") + Result.GetErrorJson());
			return false;
		}
		return true;
	};
	auto Read = [&]()
	{
		return SubmitPublic(
			Registry, Dispatch, TEXT("reference.asset_read"),
			FString::Printf(TEXT("{\"object_path\":\"%s\"}"), *ObjectPath));
	};

	const auto Create = SubmitPublic(
		Registry, Dispatch, TEXT("reference.asset_apply"),
		Params(ObjectPath, TEXT("create"), TEXT("t43-created")));
	if (!RequireSuccess(TEXT("public create"), Create))
	{
		CleanupOwnedFixture(ObjectPath);
		return false;
	}
	const auto CreatedRead = Read();
	TestTrue(TEXT("public read observes the created asset"), CreatedRead.IsOk());
	TestTrue(TEXT("public read observes the created value"),
		CreatedRead.GetPayloadJson().Contains(TEXT("t43-created")));
	TestTrue(TEXT("create publishes the package artifact"),
		!Filename.IsEmpty() && IFileManager::Get().FileExists(*Filename));

	const auto Update = SubmitPublic(
		Registry, Dispatch, TEXT("reference.asset_apply"),
		Params(ObjectPath, TEXT("update"), TEXT("t43-updated")));
	if (!RequireSuccess(TEXT("public update"), Update))
	{
		CleanupOwnedFixture(ObjectPath);
		return false;
	}
	const auto UpdatedRead = Read();
	TestTrue(TEXT("public read observes the updated asset"), UpdatedRead.IsOk());
	TestTrue(TEXT("public read observes the updated value"),
		UpdatedRead.GetPayloadJson().Contains(TEXT("t43-updated")));

	const auto Invalid = SubmitPublic(
		Registry, Dispatch, TEXT("reference.asset_apply"),
		Params(ObjectPath, TEXT("invalid"), TEXT("must-not-persist")));
	TestFalse(TEXT("invalid intent is rejected before success"), Invalid.IsOk());
	const auto UnchangedRead = Read();
	TestTrue(TEXT("failed intent leaves the current asset readable"),
		UnchangedRead.IsOk());
	TestTrue(TEXT("failed intent leaves the persisted value unchanged"),
		UnchangedRead.GetPayloadJson().Contains(TEXT("t43-updated"))
			&& !UnchangedRead.GetPayloadJson().Contains(TEXT("must-not-persist")));

	const auto Delete = SubmitPublic(
		Registry, Dispatch, TEXT("reference.asset_apply"),
		Params(ObjectPath, TEXT("delete")));
	if (!RequireSuccess(TEXT("public delete"), Delete))
	{
		CleanupOwnedFixture(ObjectPath);
		return false;
	}
	const auto DeletedRead = Read();
	TestTrue(TEXT("public read returns a valid empty snapshot after delete"),
		DeletedRead.IsOk());
	TestTrue(TEXT("deleted readback contains no persisted target entry"),
		DeletedRead.GetPayloadJson().Contains(TEXT("\"entries\":[]"))
			&& !DeletedRead.GetPayloadJson().Contains(*ObjectPath));
	TestFalse(TEXT("delete removes the package artifact"),
		!Filename.IsEmpty() && IFileManager::Get().FileExists(*Filename));
	CleanupOwnedFixture(ObjectPath);
	return !HasAnyErrors();
}

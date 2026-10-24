#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Utilities/AuthoringOperationApply/UeAgentApplyInputContract.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceRouter.h"

namespace UE::UAI::Tests::AuthoringOperationApply
{
	using namespace UeAgentApplyInputContract;
	using namespace UeAgentAuthoringWorkspaceRouter;
	using UeAgentAuthoringOperationApply::FOperationFailure;

	const FString TestContractId =
		TEXT("uai.apply_input.t130_workspace_resolution");

	FContract MakeTestContract()
	{
		FContract Contract;
		Contract.Schema = TEXT("uai.apply_input_contract.v1");
		Contract.ContractId = TestContractId;
		Contract.ContractVersion = TEXT("1.0.0");
		Contract.ProfileId = TEXT("t130.workspace-resolution");
		Contract.TargetKind = TEXT("fixture");
		Contract.SemanticDescriptorRefs = {TEXT("fixture.target")};
		Contract.ReflectionBaseline.RootKinds = {TEXT("uobject"), TEXT("ustruct")};
		Contract.ReflectionBaseline.RecursiveNodes = {
			TEXT("scalar"), TEXT("struct"), TEXT("owned_object"),
			TEXT("reference"), TEXT("array"), TEXT("set"), TEXT("map"),
			TEXT("fixed_array"), TEXT("optional")};
		Contract.ReflectionBaseline.UnknownFieldPolicy = TEXT("reject");
		Contract.ReflectionBaseline.ExternalReferencePolicy =
			TEXT("replace_reference_only");
		Contract.ReflectionBaseline.OwnedObjectPolicy =
			TEXT("create_or_locate_with_real_outer_then_recurse");
		Contract.ReflectionBaseline.RuntimeLimitsSource =
			TEXT("shared_runtime_limits");
		Contract.PropertyPayload.Mode = TEXT("reflection");
		Contract.PropertyPayload.PropertyPathField = TEXT("property_path");
		Contract.PropertyPayload.ValueField = TEXT("value");
		Contract.PropertyPayload.OmittedPatchFieldPolicy = TEXT("preserve_current");
		Contract.InputFields = {
			{TEXT("name"), TEXT("identity"), TEXT("string")},
			{TEXT("mode"), TEXT("provider_extension"), TEXT("string")},
		};
		Contract.Operations = {
			{EOperation::Create, {{TEXT("name")}},
				{TEXT("target"), TEXT("properties"), TEXT("provider_extension")},
				TEXT("ue_native_default"), TEXT("allowed")},
		};
		Contract.ProviderId = TEXT("t130-provider");
		Contract.UnknownInputFieldPolicy = TEXT("reject");
		Contract.Verification.StateSource = TEXT("shared_state_snapshot");
		Contract.Verification.CompareSource = TEXT("shared_semantic_compare");
		return Contract;
	}

	TSharedRef<FJsonObject> MakeDocument(const FString& Operation)
	{
		const TSharedRef<FJsonObject> Target = MakeShared<FJsonObject>();
		Target->SetStringField(TEXT("name"), TEXT("fixture-object"));
		const TSharedRef<FJsonObject> Extension = MakeShared<FJsonObject>();
		Extension->SetStringField(TEXT("mode"), TEXT("nested"));
		const TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
		Document->SetStringField(TEXT("operation"), Operation);
		Document->SetObjectField(TEXT("target"), Target);
		Document->SetArrayField(
			TEXT("properties"),
			{MakeShared<FJsonValueObject>(MakeShared<FJsonObject>())});
		Document->SetObjectField(TEXT("provider_extension"), Extension);
		return Document;
	}

	FWorkspaceUnit MakeUnit(const FString& ContractId, const FString& Operation)
	{
		FWorkspaceUnit Unit;
		Unit.UnitId = TEXT("fixture-unit");
		Unit.CanonicalUnitKey = TEXT("fixture-unit-key");
		Unit.ApplyContractRef = ContractId;
		Unit.Action = EUnitAction::Apply;
		Unit.Document = MakeDocument(Operation);
		return Unit;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentWorkspaceOperationInputResolutionTest,
	"UAI.Root.AuthoringOperationApply.WorkspaceOperationInputResolution",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentWorkspaceOperationInputResolutionTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Tests::AuthoringOperationApply;
	(void)Parameters;
	FContract Contract;
	const FContract TestContract = MakeTestContract();
	ERegistrationResult RegistrationResult;
	FString RegistrationError;
	TestTrue(TEXT("test contract registers"),
		FRegistry::Get().RegisterContract(
			TestContract, RegistrationResult, RegistrationError));
	if (!RegistrationError.IsEmpty())
	{
		AddError(RegistrationError);
	}

	FOperationFailure Failure;
	FOperationInput Input;
	const FWorkspaceUnit Unit = MakeUnit(TestContractId, TEXT("create"));
	TestTrue(TEXT("public helper resolves a workspace operation"),
		UeAgentAuthoringOperationApply::TryResolveWorkspaceOperationInput(
			Unit, FRegistry::Get(), Contract, Input, Failure));
	TestEqual(TEXT("resolved contract is the registered contract"),
		Contract, TestContract);
	TestEqual(TEXT("requested operation is create"),
		Input.RequestedOperation, EOperation::Create);
	TestEqual(TEXT("resolved operation is create"),
		Input.ResolvedOperation, EOperation::Create);
	TestTrue(TEXT("target is preserved"),
		Input.Target.IsValid()
			&& Input.Target->GetStringField(TEXT("name")) == TEXT("fixture-object"));
	TestEqual(TEXT("properties are preserved"), Input.Properties.Num(), 1);
	TestTrue(TEXT("provider extension is preserved"),
		Input.ProviderExtension.IsValid()
			&& Input.ProviderExtension->GetStringField(TEXT("mode")) == TEXT("nested"));

	FContract UnknownContract;
	FOperationInput UnknownInput;
	Failure = FOperationFailure();
	const FWorkspaceUnit UnknownUnit = MakeUnit(
		TEXT("uai.apply_input.t130.unknown"), TEXT("create"));
	TestFalse(TEXT("unknown contract is rejected"),
		UeAgentAuthoringOperationApply::TryResolveWorkspaceOperationInput(
			UnknownUnit, FRegistry::Get(), UnknownContract, UnknownInput, Failure));
	TestEqual(TEXT("unknown contract has typed failure"),
		Failure.Diagnostic.Code, FString(TEXT("operation_contract_not_registered")));

	FContract InvalidOperationContract;
	FOperationInput InvalidOperationInput;
	Failure = FOperationFailure();
	const FWorkspaceUnit InvalidOperationUnit = MakeUnit(
		TestContractId, TEXT("update"));
	TestFalse(TEXT("operation absent from contract is rejected"),
		UeAgentAuthoringOperationApply::TryResolveWorkspaceOperationInput(
			InvalidOperationUnit, FRegistry::Get(), InvalidOperationContract,
			InvalidOperationInput, Failure));
	TestEqual(TEXT("invalid operation has typed failure"),
		Failure.Diagnostic.Code, FString(TEXT("operation_not_allowed")));

	return !HasAnyErrors();
}

#include "Misc/AutomationTest.h"

#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUeAgentApplyFailureProjectionTest,
    "UAI.Root.AuthoringOperationApply.FailureProjection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentApplyFailureProjectionTest::RunTest(const FString &Parameters) {
  using namespace UeAgentAuthoringOperationApply;
  using namespace UeAgentAuthoringWorkspaceTransaction;
  (void)Parameters;

  FExecutionResult Execution;
  Execution.Status = TEXT("failed");
  Execution.Phase = TEXT("mutation");
  Execution.Diagnostic.CauseCode = TEXT("mutation_failed");
  Execution.Diagnostic.PublicMessage = TEXT("fixture-component");
  Execution.Diagnostic.Code = TEXT("level_component_already_exists");
  Execution.Diagnostic.Stage = TEXT("workspace_mutation");
  Execution.Diagnostic.PropertyPath = TEXT("/components/0");

  const FApplyFailureProjection ExecutionProjection =
      ProjectApplyFailure(Execution);
  TestEqual(TEXT("provider diagnostic is the public error code"),
            ExecutionProjection.Diagnostic.Code,
            FString(TEXT("level_component_already_exists")));
  TestEqual(TEXT("transaction trigger remains available as the cause"),
            ExecutionProjection.Diagnostic.CauseCode,
            FString(TEXT("mutation_failed")));
  TestEqual(TEXT("execution phase is preserved"), ExecutionProjection.Phase,
            FString(TEXT("mutation")));
  TestEqual(TEXT("failure detail is preserved"),
            ExecutionProjection.Diagnostic.PublicMessage,
            FString(TEXT("fixture-component")));
  TestEqual(TEXT("diagnostic property path is preserved"),
            ExecutionProjection.Diagnostic.PropertyPath,
            FString(TEXT("/components/0")));

  FPreflightResult Preflight;
  Preflight.Diagnostic.Code = TEXT("operation_not_allowed");
  Preflight.Diagnostic.PublicMessage = TEXT("remove");
  const FApplyFailureProjection PreflightProjection =
      ProjectApplyFailure(Preflight);
  TestTrue(TEXT("preflight projection is identified"),
           PreflightProjection.bPreflight);
  TestEqual(TEXT("preflight preserves the producer's public error code"),
            PreflightProjection.Diagnostic.Code,
            FString(TEXT("operation_not_allowed")));
  TestEqual(TEXT("preflight phase is normalized"), PreflightProjection.Phase,
            FString(TEXT("preflight")));

  FApplyWorkspaceResult ApplyResult;
  ApplyResult.Execution.Status = TEXT("failed");
  ApplyResult.Execution.Phase = Execution.Phase;
  ApplyResult.Execution.Diagnostic = Execution.Diagnostic;
  FApplyFailureProjection CombinedProjection;
  TestTrue(TEXT("combined apply result projects the execution failure"),
           TryProjectApplyFailure(ApplyResult, CombinedProjection));
  TestEqual(TEXT("combined projection preserves the provider code"),
            CombinedProjection.Diagnostic.Code, ExecutionProjection.Diagnostic.Code);

  FExecutionResult GenericExecution;
  GenericExecution.Status = TEXT("failed");
  GenericExecution.Diagnostic.Code = TEXT("workspace_transaction_failed");
  const FApplyFailureProjection GenericProjection =
      ProjectApplyFailure(GenericExecution);
  TestEqual(TEXT("workspace error is the final fallback"),
            GenericProjection.Diagnostic.Code,
            FString(TEXT("workspace_transaction_failed")));

  return !HasAnyErrors();
}

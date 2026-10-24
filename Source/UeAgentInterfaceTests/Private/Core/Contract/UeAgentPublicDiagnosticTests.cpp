#include "Core/Contract/UeAgentDiagnostic.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonValue.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUaiPublicDiagnosticTransferTest,
    "UAI.Contract.PublicDiagnosticTransfer", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUaiPublicDiagnosticTransferTest::RunTest(const FString&)
{
    using namespace UE::UAI::Contract;
    auto Original = FDiagnostic::Failure(TEXT("asset_missing"), TEXT("/Game/不存在.Asset"), TEXT("target_resolution"),
        TEXT("private context"), TEXT("private-id"));
    Original.bRetrySafe = false;
    Original.TargetPath = TEXT("/Game/Requested.Asset");
    const auto Public = Original.ToPublicJson();
    TestEqual(TEXT("actionable target remains visible"), Public->GetStringField(TEXT("message")), FString(TEXT("/Game/不存在.Asset")));
    TestFalse(TEXT("custody not copied into diagnostic"), Public->HasField(TEXT("subject_id")));
    TestFalse(TEXT("explicit retry fact survives projection"), Public->GetBoolField(TEXT("retry_safe")));
    TestEqual(TEXT("public input target survives private subject projection"), Public->GetStringField(TEXT("target")), Original.TargetPath);
    TestFalse(TEXT("internal context remains private"), Original.ToPublicDetail().Contains(TEXT("private context")));

    auto Authoring = FDiagnostic::Failure(TEXT("invalid_value"), TEXT("Wrong value"), TEXT("preflight"));
    Authoring.SuggestedFix = TEXT("Use the exported type");
    TestEqual(TEXT("suggestion survives projection"), Authoring.ToPublicJson()->GetStringField(TEXT("suggested_fix")), Authoring.SuggestedFix);

    FJsonObject Source;
    auto Location = MakeShared<FJsonObject>();
    Location->SetStringField(TEXT("source_file"), TEXT("component_tree.hjson"));
    Location->SetStringField(TEXT("json_pointer"), TEXT("/components/DefaultSceneRoot"));
    Source.SetObjectField(TEXT("authoring_source"), Location);
    auto Removed = FDiagnostic::Failure(TEXT("blueprint_component_native_remove_failed"),
        TEXT("DefaultSceneRoot"), TEXT("component_apply"));
    UeAgentAuthoringDiagnostics::LocateFailure(Removed, Source, TEXT("component_apply"), TEXT("/Game/Test.BP"));
    TestEqual(TEXT("located reason is public"), Removed.ToPublicJson()->GetStringField(TEXT("message")), FString(TEXT("DefaultSceneRoot")));
    TestEqual(TEXT("located source retained"), Removed.Source.SourceFile, FString(TEXT("component_tree.hjson")));
    UeAgentAuthoringDiagnostics::SetRollbackFacts(Removed, true);
    TestEqual(TEXT("rollback preserves reason"), Removed.PublicMessage, FString(TEXT("DefaultSceneRoot")));
    TestTrue(TEXT("rollback fact retained"), Removed.ToPublicJson()->GetBoolField(TEXT("rollback_succeeded")));

    auto InputDiagnostic = FDiagnostic::Failure(TEXT("input_invalid"), TEXT("Wrong type"), TEXT("preflight"));
    InputDiagnostic.Schema = TEXT("uai.input.diagnostic/1");
    InputDiagnostic.SchemaExpected = MakeShared<FJsonValueNumberString>(TEXT("9007199254740993"));
    auto TypeIssue = FDiagnostic::Failure(TEXT("type"));
    TypeIssue.Source.JsonPath = TEXT("/commands/2");
    InputDiagnostic.Diagnostics.Add(TypeIssue);
    auto Unchecked = MakeShared<FJsonObject>();
    Unchecked->SetStringField(TEXT("reason"), TEXT("dependency"));
    InputDiagnostic.Unchecked.Add(MakeShared<FJsonValueObject>(Unchecked));
    auto Foreign = FDiagnostic::Failure(TEXT("apply_failed"), TEXT("The input could not be applied."), TEXT("apply"));
    Foreign.PublicCause = MakeShared<FDiagnostic>(InputDiagnostic);
    TestEqual(TEXT("foreign carrier keeps own code"), Foreign.ToPublicJson()->GetObjectField(TEXT("cause"))->GetStringField(TEXT("code")), FString(TEXT("input_invalid")));
    TestTrue(TEXT("foreign carrier preserves exact integer"), Foreign.ToPublicDetail().Contains(TEXT("9007199254740993")));
    auto RawPrivate = FDiagnostic::Failure(TEXT("failed"), TEXT("The operation failed."), TEXT("apply"), TEXT("{\"private_id\":\"secret\"}"));
    TestFalse(TEXT("arbitrary machine object not exposed"), RawPrivate.ToPublicDetail().Contains(TEXT("secret")));
    return true;
}

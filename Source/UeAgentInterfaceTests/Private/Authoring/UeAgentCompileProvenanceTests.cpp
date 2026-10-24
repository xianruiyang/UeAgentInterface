#include "Misc/AutomationTest.h"
#include "Domains/Niagara/UeAgentNiagaraCompileValidation.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "NiagaraScript.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUaiCompileProvenanceTest,
    "UeAgentInterface.Common.CompileProvenance",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUaiCompileProvenanceTest::RunTest(const FString&)
{
    // Deliberately constructed native events test collection and transport.
    // The public Blueprint sequence separately exercises a real failed compile.
    UNiagaraScript* Script = NewObject<UNiagaraScript>();
    auto& Data = Script->GetVMExecutableData();
    FNiagaraCompileEvent Error;
    Error.Severity = FNiagaraCompileEventSeverity::Error;
    Error.Message = TEXT("Unbound test input");
    Error.NodeGuid = FGuid::NewGuid();
    Error.PinGuid = FGuid::NewGuid();
    Error.StackGuids = {FGuid::NewGuid(), FGuid::NewGuid()};
    Data.LastCompileEvents.Add(Error);
    FNiagaraCompileEvent Warning;
    Warning.Severity = FNiagaraCompileEventSeverity::Warning;
    Warning.Message = TEXT("Test warning");
    Data.LastCompileEvents.Add(Warning);

    const auto Compact = UeAgentNiagaraCompileValidation::InspectScripts({Script}, true, false);
    if (!TestTrue(TEXT("native error is detected without optional query diagnostics"), Compact.bHasError)
        || !TestEqual(TEXT("compact failures retain only error events"), Compact.Diagnostics.Num(), 1)) return false;
    const auto& Event = Compact.Diagnostics[0];
    TestEqual(TEXT("script provides native identity scope"), Event.ScriptPath, Script->GetPathName());
    TestEqual(TEXT("node source survives collection"), Event.NodeGuid, Error.NodeGuid);
    TestEqual(TEXT("pin source survives collection"), Event.PinGuid, Error.PinGuid);
    TestTrue(TEXT("native call stack order survives collection"), Event.StackGuids == Error.StackGuids);
    const auto Detailed = UeAgentNiagaraCompileValidation::InspectScripts({Script}, true, true);
    TestEqual(TEXT("optional diagnostics include the warning"), Detailed.Diagnostics.Num(), 2);

    auto Context = MakeShared<FJsonObject>();
    Context->SetStringField(TEXT("source_kind"), TEXT("native_compile_event"));
    Context->SetStringField(TEXT("script_path"), Event.ScriptPath);
    Context->SetStringField(TEXT("node_guid"), Event.NodeGuid.ToString());
    Context->SetStringField(TEXT("pin_guid"), Event.PinGuid.ToString());
    TArray<TSharedPtr<FJsonValue>> Stack;
    for (const FGuid& Guid : Event.StackGuids) Stack.Add(MakeShared<FJsonValueString>(Guid.ToString()));
    Context->SetArrayField(TEXT("stack_guids"), Stack);
    auto Message = MakeShared<FJsonObject>();
    Message->SetStringField(TEXT("message"), Event.Message);
    Message->SetArrayField(TEXT("related_sources"), {MakeShared<FJsonValueObject>(Context)});
    const auto Failure = UeAgentAuthoringDiagnostics::MakeCompileFailure(
        TEXT("niagara_compile_error"), Event.ScriptPath, {MakeShared<FJsonValueObject>(Message)});
    const auto Actual = Failure.ToPublicJson()->GetArrayField(TEXT("diagnostics"))[0]->AsObject()
        ->GetArrayField(TEXT("related_sources"))[0]->AsObject();
    TestEqual(TEXT("native scope survives common carrier"), Actual->GetStringField(TEXT("script_path")), Event.ScriptPath);
    TestEqual(TEXT("native node survives common carrier"), Actual->GetStringField(TEXT("node_guid")), Error.NodeGuid.ToString());
    TestEqual(TEXT("native pin survives common carrier"), Actual->GetStringField(TEXT("pin_guid")), Error.PinGuid.ToString());
    TestEqual(TEXT("native call stack survives common carrier"), Actual->GetArrayField(TEXT("stack_guids"))[1]->AsString(), Error.StackGuids[1].ToString());
    TestFalse(TEXT("native context does not invent a file location"), Actual->HasField(TEXT("source_file")));

    Data.LastCompileEvents.Reset();
    const auto Empty = UeAgentNiagaraCompileValidation::InspectScripts({Script}, true, false);
    TestFalse(TEXT("clearing native errors clears observed failure"), Empty.bHasError);
    TestTrue(TEXT("normal compact observation has no event payload"), Empty.Diagnostics.IsEmpty());
    Data.ErrorMsg = TEXT("Native message without node provenance");
    const auto Fallback = UeAgentNiagaraCompileValidation::InspectScripts({Script}, true, false);
    if (!TestEqual(TEXT("native error text fallback remains visible"), Fallback.Diagnostics.Num(), 1)) return false;
    TestEqual(TEXT("fallback retains native text"), Fallback.Diagnostics[0].Message, Data.ErrorMsg);
    TestFalse(TEXT("fallback does not invent node identity"), Fallback.Diagnostics[0].NodeGuid.IsValid());
    return true;
}

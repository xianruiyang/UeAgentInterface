#include "Utilities/AuthoringExport/UeAgentExportWorkspaceWriter.h"

#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUeAgentExportWorkspaceResultProjectionTest,
    "UeAgentInterface.Core.HjsonWorkspace.ExportWorkspaceResultProjection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentExportWorkspaceResultProjectionTest::RunTest(
    const FString &Parameters) {
  UeAgentExportWorkspaceWriter::FWorkspaceResult Result;
  Result.Status = TEXT("written");
  Result.ContractId = TEXT("uai.export.test");
  Result.ContractVersion = TEXT("1.0.0");
  Result.ProfileId = TEXT("llm_authoring");
  Result.ExpectedRevision = TEXT("capture:0123456789abcdef01234567");
  Result.WorkspaceRoot = TEXT("C:/Temp/UAI/Test");
  Result.EntryFile = TEXT("index.hjson");
  Result.Files.SetNum(2);
  Result.WrittenFiles = {TEXT("C:/Temp/UAI/Test/index.hjson"),
                         TEXT("C:/Temp/UAI/Test/items/a.hjson")};
  Result.WrittenRelativeFiles = {TEXT("index.hjson"),
                                 TEXT("items/a.hjson")};
  Result.ModelMetrics.CharacterCount = 128;
  Result.ModelMetrics.TokenProxy = 32;

  const TSharedRef<FJsonObject> InternalJson = Result.ToJsonObject();
  TestEqual(TEXT("Internal evidence retains the planned file count"),
            InternalJson->GetIntegerField(TEXT("file_count")), 2);
  TestEqual(TEXT("Internal evidence retains the written file count"),
            InternalJson->GetIntegerField(TEXT("written_file_count")), 2);
  TestEqual(TEXT("Internal evidence retains the canonical revision"),
            InternalJson->GetStringField(TEXT("expected_revision")),
            Result.ExpectedRevision);
  TestEqual(TEXT("Internal evidence retains the model token metric"),
            InternalJson->GetObjectField(TEXT("model_metrics"))
                ->GetIntegerField(TEXT("token_proxy")),
            32);
  TestFalse(TEXT("Internal summary omits planned file details"),
            InternalJson->HasField(TEXT("files")));
  TestFalse(TEXT("Internal summary omits absolute written paths"),
            InternalJson->HasField(TEXT("written_files")));
  TestFalse(TEXT("Internal summary omits relative written paths"),
            InternalJson->HasField(TEXT("written_relative_files")));

  const TSharedRef<FJsonObject> PublicJson = Result.ToPublicJsonObject();
  TestEqual(TEXT("Public export result exposes one business field"),
            PublicJson->Values.Num(), 1);
  TestEqual(TEXT("A complete published export reports succeeded"),
            PublicJson->GetStringField(TEXT("status")), TEXT("succeeded"));
  TestFalse(TEXT("Public export result hides the canonical revision"),
            PublicJson->HasField(TEXT("expected_revision")));
  TestFalse(TEXT("Public export result hides metrics"),
            PublicJson->HasField(TEXT("model_metrics")));
  TestFalse(TEXT("Public export result does not repeat the workspace path"),
            PublicJson->HasField(TEXT("workspace_root")));
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUeAgentCompoundWorkspaceMetricsProjectionTest,
    "UeAgentInterface.Core.HjsonWorkspace.CompoundWorkspaceMetricsProjection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentCompoundWorkspaceMetricsProjectionTest::RunTest(
    const FString &Parameters) {
  (void)Parameters;
  using namespace UeAgentExportWorkspaceWriter;

  FPlannedFile File;
  File.FileId = TEXT("entry");
  File.RelativePath = TEXT("index.hjson");
  File.Role = TEXT("state");
  File.HjsonText = TEXT("{\n  value: 1\n}\n");
  FWorkspaceFragment Fragment;
  Fragment.FragmentId = TEXT("fixture");
  Fragment.Files.Add(MoveTemp(File));

  FCompoundPlanPolicy Policy;
  Policy.ContractId = TEXT("uai.fixture.compound");
  Policy.ContractVersion = TEXT("1");
  Policy.ProfileId = TEXT("llm_authoring");
  Policy.EntryFile = TEXT("index.hjson");

  FWorkspaceResult Result;
  const bool bBuilt = BuildCompoundWorkspacePlan(
      FPaths::Combine(FPaths::ProjectSavedDir(),
                      TEXT("Automation/CompoundMetrics")),
      {Fragment}, Policy, Result);
  TestTrue(TEXT("compound plans measure fragment HJSON text"), bBuilt);
  if (!bBuilt) {
    AddError(Result.ErrorCode + TEXT(":") + Result.ErrorDetail);
    return false;
  }
  TestEqual(TEXT("compound plan reports physical lines"),
            Result.Metrics.LineCount, int64(3));
  TestEqual(TEXT("compound plan reports text characters"),
            Result.Metrics.CharacterCount, int64(15));
  TestTrue(TEXT("model metrics inherit measured lines"),
           Result.ModelMetrics.LineCount > 0);

	const FString TextWithoutTrailingNewline =
		TEXT("{\n  label: \"蓝图\"\n}");
	const FTCHARToUTF8 Utf8WithoutTrailingNewline(
		*TextWithoutTrailingNewline);
	const FTextMetrics Utf8Metrics = MeasureUtf8Text(
		MakeArrayView(
			reinterpret_cast<const uint8*>(Utf8WithoutTrailingNewline.Get()),
			Utf8WithoutTrailingNewline.Length()));
	TestEqual(TEXT("UTF-8 metrics count physical lines without a trailing newline"),
		Utf8Metrics.LineCount, int64(3));
	TestTrue(TEXT("UTF-8 byte metrics preserve multibyte text cost"),
		Utf8Metrics.Utf8Bytes > Utf8Metrics.CharacterCount);
  return true;
}

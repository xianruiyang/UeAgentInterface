#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraphNode.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundNodeDelay.h"
#include "Sound/SoundNodeMixer.h"
#include "TestSupport/UeAgentPublicCommandTestHarness.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"
#include "Vertical/T85/UeAgentT85FixtureLifecycle.h"

namespace {
bool LoadVisibleSoundCueGraph(const FString &Directory, FString &OutFile,
                             TSharedPtr<FJsonObject> &OutGraph, FString &OutError) {
  TArray<FString> Files;
  IFileManager::Get().FindFilesRecursive(Files, *Directory, TEXT("*.hjson"), true, false);
  Files.Sort();
  for (const FString &File : Files) {
    if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\")) ||
        File.EndsWith(TEXT(".interfaces.hjson"))) continue;
    FString Text;
    TSharedPtr<FJsonObject> Document;
    if (!FFileHelper::LoadFileToString(Text, *File) ||
        !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Document, OutError)) return false;
    const TSharedPtr<FJsonObject> *Graph = nullptr;
    FString Semantic;
    if (Document->TryGetObjectField(TEXT("graph"), Graph) && Graph && Graph->IsValid() &&
        (*Graph)->TryGetStringField(TEXT("semantic"), Semantic) && Semantic == TEXT("sound_cue_graph")) {
      const TSharedPtr<FJsonObject> *References = nullptr;
      FString Root;
      if (!Document->HasTypedField<EJson::Object>(TEXT("nodes")) ||
          !Document->HasTypedField<EJson::Array>(TEXT("connections")) ||
          !(*Graph)->TryGetObjectField(TEXT("node_references"), References) || !References || !References->IsValid() ||
          !(*References)->TryGetStringField(TEXT("root"), Root)) {
        OutError = TEXT("sound_cue_visible_graph_shape_invalid");
        return false;
      }
      if (Text.Contains(TEXT("class_path")) || Text.Contains(TEXT("node_guid")) ||
          Text.Contains(TEXT("native_class_ref")) || Text.Contains(TEXT("root_node_id"))) {
        OutError = TEXT("sound_cue_visible_graph_contains_machine_fields");
        return false;
      }
      OutFile = File;
      OutGraph = Document;
      return true;
    }
  }
  OutError = TEXT("sound_cue_visible_graph_not_found");
  return false;
}

bool WriteVisibleSoundCueGraph(const FString &File, const TSharedRef<FJsonObject> &Document, FString &Error) {
  FString Text;
  return FUeAgentStructuredHjsonFormatter::TryWriteObject(Document, Text, Error) &&
      FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208SoundCueWorkspaceTest,
    "UAI.Root.T208.SoundCue.WorkspaceGraph",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208SoundCueWorkspaceTest::RunTest(const FString &) {
  using namespace UE::UAI;
  const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
  const FString PackageName = TEXT("/Game/UAI/Transient/T208SoundCue/SC_") + Token;
  const FString ObjectPath = PackageName + TEXT(".") + FPackageName::GetLongPackageAssetName(PackageName);
  const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
  const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-soundcue/native") / Token);
  FString Error;
  TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
  TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
  if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) {
    AddError(Error); return false;
  }
  auto Submit = [&](const FString &Command, const TSharedRef<FJsonObject> &Args) {
    return Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, Command, Args,
        TEXT("t208-sound-cue"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
  };
  // Native construction establishes only the persisted fixture, not the editing path.
  USoundCue *Cue = NewObject<USoundCue>(CreatePackage(*PackageName),
      *FPackageName::GetLongPackageAssetName(PackageName), RF_Public | RF_Standalone);
  Cue->CreateGraph();
  USoundNodeMixer *Mixer = Cue->ConstructSoundNode<USoundNodeMixer>(USoundNodeMixer::StaticClass(), false);
  USoundNodeDelay *First = Cue->ConstructSoundNode<USoundNodeDelay>(USoundNodeDelay::StaticClass(), false);
  USoundNodeDelay *Second = Cue->ConstructSoundNode<USoundNodeDelay>(USoundNodeDelay::StaticClass(), false);
  if (!TestNotNull(TEXT("fixture mixer"), Mixer) || !TestNotNull(TEXT("fixture first delay"), First) ||
      !TestNotNull(TEXT("fixture second delay"), Second)) return false;
  Mixer->CreateStartingConnectors(); First->CreateStartingConnectors(); Second->CreateStartingConnectors();
  TArray<USoundNode *> InitialChildren = {First, Second};
  Mixer->SetChildNodes(InitialChildren);
  Mixer->InputVolume = {0.25f, 0.75f};
  First->DelayMin = 0.125f; First->DelayMax = 0.25f;
  Second->DelayMin = 0.5f; Second->DelayMax = 0.75f;
  Cue->VolumeMultiplier = 0.625f;
  Cue->FirstNode = Mixer;
  Mixer->GetGraphNode()->ReconstructNode();
  Mixer->GetGraphNode()->NodePosX = 0;
  First->GetGraphNode()->NodePosX = 128;
  Second->GetGraphNode()->NodePosX = 256;
  Cue->LinkGraphNodesFromSoundNodes(); Cue->CompileSoundNodesFromGraphNodes();
  const FString MixerName = Mixer->GetName(), FirstName = First->GetName(), SecondName = Second->GetName();
  const FGuid MixerGuid = Mixer->GetGraphNode()->NodeGuid;
  const FGuid FirstGuid = First->GetGraphNode()->NodeGuid;
  const FGuid SecondGuid = Second->GetGraphNode()->NodeGuid;
  FSavePackageArgs Save; Save.TopLevelFlags = RF_Public | RF_Standalone; Save.SaveFlags = SAVE_NoError;
  ON_SCOPE_EXIT {
    if (auto *Current = FindObject<USoundCue>(nullptr, *ObjectPath); Current && Current->GetPackage()->IsDirty())
      TestTrue(TEXT("settle only owned fixture after assertions"), UPackage::SavePackage(Current->GetPackage(), Current, *Filename, Save));
    if (FPackageName::DoesPackageExist(PackageName)) {
      auto Delete = MakeShared<FJsonObject>();
      Delete->SetArrayField(TEXT("targets"), {MakeShared<FJsonValueString>(ObjectPath)});
      Delete->SetBoolField(TEXT("recursive"), false);
      for (bool Confirm : {false, true}) {
        Delete->SetBoolField(TEXT("confirm"), Confirm);
        const auto Result = Submit(TEXT("asset.delete"), Delete);
        if (!Result.IsOk()) AddError(Result.GetErrorJson());
      }
      TestFalse(TEXT("owned fixture removed"), FPackageName::DoesPackageExist(PackageName));
    }
  };
  if (!TestTrue(TEXT("persistent SoundCue fixture saved"), UPackage::SavePackage(Cue->GetPackage(), Cue, *Filename, Save))) return false;
  auto Export = [&](const FString &Stage) {
    auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), ObjectPath);
    Args->SetStringField(TEXT("workspace_path"), Directory / Stage);
    Args->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
    const auto Result = Submit(TEXT("content_graph.export_workspace"), Args);
    if (!Result.IsOk()) AddError(Result.GetErrorJson());
    return Result.IsOk();
  };
  auto Import = [&](const FString &Stage) {
    auto Args = MakeShared<FJsonObject>(); auto Source = MakeShared<FJsonObject>();
    Args->SetStringField(TEXT("target"), ObjectPath); Source->SetStringField(TEXT("kind"), TEXT("directory"));
    Source->SetStringField(TEXT("path"), Directory / Stage); Args->SetObjectField(TEXT("source"), Source);
    return Submit(TEXT("content_graph.import_workspace"), Args);
  };
  auto CandidateArgs = MakeShared<FJsonObject>();
  CandidateArgs->SetStringField(TEXT("target"), ObjectPath); CandidateArgs->SetStringField(TEXT("query"), TEXT("Mixer"));
  CandidateArgs->SetNumberField(TEXT("limit"), 3);
  const auto CandidateResult = Submit(TEXT("audio.graph.candidates"), CandidateArgs);
  if (!TestTrue(TEXT("public candidate query"), CandidateResult.IsOk())) { AddError(CandidateResult.GetErrorJson()); return false; }
  TSharedPtr<FJsonObject> CandidatePayload;
  const TArray<TSharedPtr<FJsonValue>> *Names = nullptr;
  if (!FUeAgentStructuredHjsonParser::ParseObject(CandidateResult.GetPayloadJson(), TEXT("candidate-result"), CandidatePayload, Error) ||
      !CandidatePayload->TryGetArrayField(TEXT("names"), Names) || !Names) {
    AddError(TEXT("public Mixer query payload invalid: ") + Error); return false;
  }
  FString CandidateName;
  for (const auto &Name : *Names)
    if (Name.IsValid() && Name->AsString() == TEXT("Mixer")) CandidateName = Name->AsString();
  if (!TestFalse(TEXT("exact public Mixer candidate is available"), CandidateName.IsEmpty())) return false;
  if (!Export(TEXT("edit"))) return false;
  FString GraphFile;
  TSharedPtr<FJsonObject> Document;
  if (!LoadVisibleSoundCueGraph(Directory / TEXT("edit"), GraphFile, Document, Error)) { AddError(Error); return false; }
  const auto Nodes = Document->GetObjectField(TEXT("nodes"));
  const auto Graph = Document->GetObjectField(TEXT("graph"));
  const auto References = Graph->GetObjectField(TEXT("node_references"));
  const FString MixerAlias = References->GetStringField(TEXT("root"));
  FString FirstAlias, SecondAlias;
  for (const auto &Pair : Nodes->Values) {
    const auto Node = Pair.Value->AsObject();
    const TSharedPtr<FJsonObject> *Position = nullptr;
    double X = 0;
    if (Node->TryGetObjectField(TEXT("pos"), Position) && Position && Position->IsValid() && (*Position)->TryGetNumberField(TEXT("x"), X)) {
      if (X == 128) FirstAlias = Pair.Key;
      if (X == 256) SecondAlias = Pair.Key;
    }
  }
  if (!TestTrue(TEXT("ordinary workspace exposes existing node aliases"), !MixerAlias.IsEmpty() && !FirstAlias.IsEmpty() && !SecondAlias.IsEmpty())) return false;
  auto Connections = Document->GetArrayField(TEXT("connections"));
  FString FirstEndpoint;
  TSharedPtr<FJsonObject> SecondConnection;
  for (const auto &Value : Connections) {
    const auto Connection = Value->AsObject();
    const FString From = Connection->GetStringField(TEXT("from"));
    if (From.StartsWith(FirstAlias + TEXT("."))) FirstEndpoint = From;
    if (From.StartsWith(SecondAlias + TEXT("."))) SecondConnection = Connection;
  }
  if (!TestTrue(TEXT("visible second-input connection found"), Connections.Num() == 2 && !FirstEndpoint.IsEmpty() && SecondConnection.IsValid())) return false;
  SecondConnection->SetStringField(TEXT("from"), FirstEndpoint);
  auto Added = MakeShared<FJsonObject>(); auto Candidate = MakeShared<FJsonObject>();
  Candidate->SetStringField(TEXT("name"), CandidateName); Added->SetObjectField(TEXT("candidate"), Candidate);
  auto Position = MakeShared<FJsonObject>(); Position->SetNumberField(TEXT("x"), 640); Position->SetNumberField(TEXT("y"), 160);
  Added->SetObjectField(TEXT("pos"), Position); Nodes->SetObjectField(TEXT("AddedMixer"), Added);
  References->SetStringField(TEXT("root"), TEXT("AddedMixer"));
  if (!WriteVisibleSoundCueGraph(GraphFile, Document.ToSharedRef(), Error)) { AddError(Error); return false; }
  Mixer = nullptr; First = nullptr; Second = nullptr; Cue = nullptr;
  const auto Imported = Import(TEXT("edit"));
  if (!TestTrue(TEXT("public workspace import succeeds"), Imported.IsOk())) { AddError(Imported.GetErrorJson()); return false; }
  TSharedPtr<FJsonObject> ImportPayload;
  FString Status;
  if (!TestTrue(TEXT("public transaction reports business success"),
      FUeAgentStructuredHjsonParser::ParseObject(Imported.GetPayloadJson(), TEXT("import-result"), ImportPayload, Error) &&
      ImportPayload.IsValid() && !ImportPayload->HasField(TEXT("operation_error")) &&
      ImportPayload->TryGetStringField(TEXT("status"), Status) && Status == TEXT("succeeded"))) return false;
  Cue = FindObject<USoundCue>(nullptr, *ObjectPath);
  if (!TestTrue(TEXT("import settled the package before independent unload"), Cue && !Cue->GetPackage()->IsDirty())) return false;
  Cue = nullptr;
  if (!T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)) { AddError(Error); return false; }
  Cue = LoadObject<USoundCue>(nullptr, *ObjectPath);
  if (!TestNotNull(TEXT("independently loaded persisted cue"), Cue)) return false;
  Mixer = FindObject<USoundNodeMixer>(Cue, *MixerName);
  First = FindObject<USoundNodeDelay>(Cue, *FirstName);
  Second = FindObject<USoundNodeDelay>(Cue, *SecondName);
  if (!TestTrue(TEXT("native node identities and graph GUIDs survive the public transaction"),
      Mixer && First && Second && Mixer->GetGraphNode() && First->GetGraphNode() && Second->GetGraphNode() && Mixer->GetGraphNode()->NodeGuid == MixerGuid &&
      First->GetGraphNode()->NodeGuid == FirstGuid && Second->GetGraphNode()->NodeGuid == SecondGuid)) return false;
  TestTrue(TEXT("second input is edited without moving the untouched first input"),
      Mixer->ChildNodes.Num() == 2 && Mixer->ChildNodes[0] == First && Mixer->ChildNodes[1] == First);
  TestTrue(TEXT("properties and disconnected node survive"),
      Cue->VolumeMultiplier == 0.625f && Mixer->InputVolume == TArray<float>({0.25f, 0.75f}) &&
      First->DelayMin == 0.125f && First->DelayMax == 0.25f && Second->DelayMin == 0.5f && Second->DelayMax == 0.75f);
  TestTrue(TEXT("root points at the new candidate node after disk reload"), Cue->AllNodes.Num() == 4 &&
      Cue->FirstNode && Cue->FirstNode != Mixer && Cue->FirstNode->IsA<USoundNodeMixer>() && Cue->FirstNode->GetGraphNode() &&
      Cue->FirstNode->GetGraphNode()->NodePosX == 640 && !Cue->GetPackage()->IsDirty());
  if (!Export(TEXT("readback")) || !LoadVisibleSoundCueGraph(Directory / TEXT("readback"), GraphFile, Document, Error)) { AddError(Error); return false; }
  TestTrue(TEXT("independent visible graph retains both connections"), Document->GetArrayField(TEXT("connections")).Num() == 2);
  // Invalid candidate selection must leave both loaded state and original disk intact.
  TArray<uint8> Before, After;
  if (!FFileHelper::LoadFileToArray(Before, *Filename)) return false;
  const auto ReadbackNodes = Document->GetObjectField(TEXT("nodes"));
  auto Invalid = MakeShared<FJsonObject>(); auto InvalidCandidate = MakeShared<FJsonObject>(); auto Select = MakeShared<FJsonObject>();
  InvalidCandidate->SetStringField(TEXT("name"), CandidateName); Select->SetStringField(TEXT("source_kind"), TEXT("not-a-sound-cue-node"));
  InvalidCandidate->SetObjectField(TEXT("select"), Select); Invalid->SetObjectField(TEXT("candidate"), InvalidCandidate);
  ReadbackNodes->SetObjectField(TEXT("RejectedMixer"), Invalid);
  if (!WriteVisibleSoundCueGraph(GraphFile, Document.ToSharedRef(), Error)) { AddError(Error); return false; }
  const auto Rejected = Import(TEXT("readback"));
  TestFalse(TEXT("candidate select mismatch is rejected"), Rejected.IsOk());
  TestTrue(TEXT("rejected selection preserves original package bytes"), FFileHelper::LoadFileToArray(After, *Filename) && Before == After);
  Cue = FindObject<USoundCue>(nullptr, *ObjectPath);
  TestTrue(TEXT("rejected selection preserves loaded native graph"), Cue && Cue->AllNodes.Num() == 4 && !Cue->GetPackage()->IsDirty());
  return !HasAnyErrors();
}

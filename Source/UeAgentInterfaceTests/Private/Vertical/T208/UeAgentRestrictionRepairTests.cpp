#include "Misc/AutomationTest.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimSequence.h"
#include "Animation/BlendSpace1D.h"
#include "Animation/Skeleton.h"
#include "ControlRigBlueprint.h"
#include "Rigs/RigHierarchy.h"
#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringApplyProvider.h"
#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringRegistration.h"
#include "Domains/Material/Authoring/UeAgentMaterialAuthoringApplyProvider.h"
#include "Domains/Material/Authoring/UeAgentMaterialAuthoringRegistration.h"
#include "Domains/Rigging/Authoring/UeAgentRiggingAuthoringDocuments.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Materials/MaterialFunction.h"
#include "MovieScene.h"
#include "Tracks/MovieSceneEventTrack.h"
#include "Sections/MovieSceneEventTriggerSection.h"
#include "Utilities/AuthoringCore/UeAgentMovieSceneAuthoring.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "UObject/Package.h"

namespace {
FString RepairAssetPath(const TCHAR* Name) {
  return TEXT("/Game/UAI/Transient/RestrictionRepair_") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT("/") + Name;
}
TSharedPtr<FJsonValue> RepairProperty(const TCHAR* Path, TSharedPtr<FJsonValue> Value) {
  auto Property = MakeShared<FJsonObject>();
  Property->SetStringField(TEXT("property_path"), Path);
  Property->SetField(TEXT("value"), Value);
  return MakeShared<FJsonValueObject>(Property);
}
void RemoveRepairAsset(const FString& Path) {
  if (UObject* Object = LoadObject<UObject>(nullptr, *(Path + TEXT(".") + FPackageName::GetLongPackageAssetName(Path)))) {
    TGuardValue<bool> Guard(GIsRunningUnattendedScript, true);
    Object->GetPackage()->SetDirtyFlag(false);
    ObjectTools::DeleteObjects({Object}, false);
  }
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUaiFunctionSettingsAliasRepair,
  "UAI.Root.Hjson.RestrictionRepair.FunctionAliases", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUaiFunctionSettingsAliasRepair::RunTest(const FString&) {
  using namespace UeAgentMaterialAuthoring;
  FString Error;
  if (!TestTrue(TEXT("material authoring registration"), RegisterAuthoring(Error))) { AddError(Error); return false; }
  const FString Path = RepairAssetPath(TEXT("Function"));
  ON_SCOPE_EXIT { RemoveRepairAsset(Path); };
  auto* Function = NewObject<UMaterialFunction>(CreatePackage(*Path), TEXT("Function"), RF_Public | RF_Standalone);
  Function->Description = TEXT("before");
  FAssetApplyRequest Request;
  Request.AssetPath = Path; Request.AssetKind = TEXT("material_function"); Request.bCompile = false;
  Request.FunctionSettings = MakeShared<FJsonObject>();
  Request.FunctionSettings->SetStringField(TEXT("description"), TEXT("alias only"));
  Request.FunctionSettings->SetBoolField(TEXT("expose_to_library"), true);
  FAssetApplyResult Result;
  if (!TestTrue(TEXT("alias-only enters shared property apply"), ApplyAsset(Request, Result))) { AddError(Result.ErrorCode + TEXT(":") + Result.ErrorDetail); return false; }
  TestEqual(TEXT("alias description applied"), Function->Description, FString(TEXT("alias only")));
  TestTrue(TEXT("alias bool applied"), static_cast<bool>(Function->bExposeToLibrary));
  Request.Properties = {RepairProperty(TEXT("Description"), MakeShared<FJsonValueString>(TEXT("alias only")))};
  TestTrue(TEXT("equivalent duplicate expression coalesces"), ApplyAsset(Request, Result));
  Request.FunctionSettings->SetStringField(TEXT("description"), TEXT("conflict"));
  TestFalse(TEXT("conflicting expressions fail before writes"), ApplyAsset(Request, Result));
  TestEqual(TEXT("conflict preserves native value"), Function->Description, FString(TEXT("alias only")));
  Request.Properties.Reset(); Request.FunctionSettings->SetStringField(TEXT("description"), TEXT("persistent alias")); Request.bSave = true;
  if (!TestTrue(TEXT("alias follows common persistent readback"), ApplyAsset(Request, Result))) { AddError(Result.ErrorCode + TEXT(":") + Result.ErrorDetail); return false; }
  Function = LoadObject<UMaterialFunction>(nullptr, *(Path + TEXT(".Function")));
  TestTrue(TEXT("saved alias value and clean state"), Function && Function->Description == TEXT("persistent alias") && !Function->GetPackage()->IsDirty());
  return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUaiAnimationCreationRepair,
  "UAI.Root.Hjson.RestrictionRepair.AnimationCreation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUaiAnimationCreationRepair::RunTest(const FString&) {
  using namespace UeAgentAnimationAuthoring;
  FString Error;
  if (!RegisterAuthoring(Error)) { AddError(Error); return false; }
  const FString SequencePath = RepairAssetPath(TEXT("Sequence")), BlendPath = RepairAssetPath(TEXT("Blend"));
  ON_SCOPE_EXIT { RemoveRepairAsset(BlendPath); RemoveRepairAsset(SequencePath); };
  auto* Source = LoadObject<UAnimSequence>(nullptr,
    TEXT("/Game/Characters/PaladinRootMotion/Animations/sword_and_shield_idle_RM.sword_and_shield_idle_RM"));
  if (!TestTrue(TEXT("existing source supplies native skeleton only"), Source && Source->GetSkeleton())) return false;
  FAssetApplyRequest Request; Request.Kind = EAssetKind::AnimSequence;
  Request.Operation = UeAgentApplyInputContract::EOperation::Create; Request.AssetPath = SequencePath;
  Request.Document = MakeShared<FJsonObject>(); Request.Document->SetStringField(TEXT("skeleton"), Source->GetSkeleton()->GetPathName());
  Request.Document->SetNumberField(TEXT("number_of_frames"), 3);
  Request.Properties = {RepairProperty(TEXT("RateScale"), MakeShared<FJsonValueNumber>(1.25))};
  Request.bSave = true;
  FAssetApplyResult Result;
  if (!TestTrue(TEXT("sequence native initialization and edit in one transaction"), ApplyAsset(Request, Result))) { AddError(Result.ErrorCode + TEXT(":") + Result.ErrorDetail); return false; }
  auto* Sequence = Cast<UAnimSequence>(Result.Asset);
  TestTrue(TEXT("sequence model initialized with requested values"), Sequence && Sequence->GetDataModel()
    && Sequence->GetDataModel()->GetNumberOfFrames() == 3 && Sequence->RateScale == 1.25f);
  Request.Kind = EAssetKind::BlendSpace; Request.AssetPath = BlendPath;
  Request.Properties = {RepairProperty(TEXT("bLoop"), MakeShared<FJsonValueBoolean>(false))};
  Request.Document->RemoveField(TEXT("number_of_frames")); Request.Document->SetNumberField(TEXT("dimension"), 1);
  if (!TestTrue(TEXT("BlendSpace factory and root properties in one transaction"), ApplyAsset(Request, Result))) { AddError(Result.ErrorCode + TEXT(":") + Result.ErrorDetail); return false; }
  auto* Blend = Cast<UBlendSpace1D>(Result.Asset);
  TestTrue(TEXT("native 1D class, skeleton and common property survive"), Blend && Blend->GetSkeleton() == Source->GetSkeleton() && !Blend->bLoop);
  return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUaiRigHierarchyRepair,
  "UAI.Root.Hjson.RestrictionRepair.RigHierarchy", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUaiRigHierarchyRepair::RunTest(const FString&) {
  using namespace UeAgentRiggingAuthoring;
  FString Error;
  auto* Rig = Cast<UControlRigBlueprint>(CreateFactorySeed(EAssetKind::ControlRig, GetTransientPackage(), NAME_None, RF_Transient, Error));
  if (!TestNotNull(TEXT("native rig seed"), Rig)) { AddError(Error); return false; }
  TSharedPtr<FJsonObject> Baseline;
  if (!BuildDocument(EAssetKind::ControlRig, *Rig, Baseline, Error)) { AddError(Error); return false; }
  auto Elements = MakeShared<FJsonObject>();
  for (const TCHAR* Kind : {TEXT("null"), TEXT("connector"), TEXT("socket")}) {
    auto Element = MakeShared<FJsonObject>(); Element->SetStringField(TEXT("kind"), Kind); Element->SetStringField(TEXT("name"), Kind);
    if (FString(Kind) == TEXT("socket")) {
      auto Parent = MakeShared<FJsonObject>(); Parent->SetStringField(TEXT("kind"), TEXT("null")); Parent->SetStringField(TEXT("name"), TEXT("null"));
      Element->SetObjectField(TEXT("parent"), Parent); Element->SetStringField(TEXT("description"), TEXT("retained socket"));
    }
    Elements->SetObjectField(FString(Kind) + TEXT(":") + Kind, Element);
  }
  auto Hierarchy = MakeShared<FJsonObject>(); Hierarchy->SetStringField(TEXT("transform_space"), TEXT("parent_local_initial")); Hierarchy->SetObjectField(TEXT("elements"), Elements);
  auto Edit = MakeShared<FJsonObject>(); Edit->SetObjectField(TEXT("hierarchy"), Hierarchy);
  FMutationStats Stats;
  if (!TestTrue(TEXT("native connector and socket create"), ApplyDocument(EAssetKind::ControlRig, *Rig, Edit, Stats, Error, Baseline))) { AddError(Error); return false; }
  TSharedPtr<FJsonObject> Captured;
  if (!TestTrue(TEXT("new types export"), BuildDocument(EAssetKind::ControlRig, *Rig, Captured, Error))) { AddError(Error); return false; }
  const auto CapturedElements = Captured->GetObjectField(TEXT("hierarchy"))->GetObjectField(TEXT("elements"));
  TestTrue(TEXT("connector and socket are not filtered"), CapturedElements->HasField(TEXT("connector:connector")) && CapturedElements->HasField(TEXT("socket:socket")));
  TestEqual(TEXT("socket native metadata captured"), CapturedElements->GetObjectField(TEXT("socket:socket"))->GetStringField(TEXT("description")), FString(TEXT("retained socket")));
  Elements->RemoveField(TEXT("connector:connector"));
  Elements->GetObjectField(TEXT("socket:socket"))->SetStringField(TEXT("description"), TEXT("edited socket"));
  if (!TestTrue(TEXT("native connector removal and socket edit"), ApplyDocument(EAssetKind::ControlRig, *Rig, Edit, Stats, Error, Captured))) { AddError(Error); return false; }
  TestTrue(TEXT("connector actually removed"), Rig->GetHierarchy()->GetIndex(FRigElementKey(TEXT("connector"), ERigElementType::Connector)) == INDEX_NONE);
  return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUaiMovieSceneNativeChannelRepair,
  "UAI.Root.Hjson.RestrictionRepair.NativeChannels", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUaiMovieSceneNativeChannelRepair::RunTest(const FString&) {
  using namespace UE::UAI::Authoring::Timeline;
  auto* Scene = NewObject<UMovieScene>();
  Scene->SetDisplayRate(FFrameRate(30, 1)); Scene->SetTickResolutionDirectly(FFrameRate(30, 1)); Scene->SetPlaybackRange(-30, 120);
  auto* Track = Scene->AddTrack<UMovieSceneEventTrack>();
  auto* Section = NewObject<UMovieSceneEventTriggerSection>(Track);
  Section->SetRange(TRange<FFrameNumber>(FFrameNumber(-30), FFrameNumber(90))); Track->AddSection(*Section);
  Section->EventChannel.GetData().AddKey(FFrameNumber(12), FMovieSceneEvent());
  FUeAgentTimelineAnimationExecutionError Error;
  TSharedPtr<FJsonObject> Document;
  if (!TestTrue(TEXT("event channel does not block common structure export"), TryReadMovieSceneStructure(*Scene, TEXT("clock"), {}, Document, Error))) { AddError(Error.Code + TEXT(":") + Error.Detail); return false; }
  FTimelineAnimationStructure Current, Desired;
  if (!TestTrue(TEXT("exported native context parses"), ParseStructure(Document, Current, Error) && ParseStructure(Document, Desired, Error))) { AddError(Error.Code); return false; }
  if (!TestTrue(TEXT("native channel has read-only context"), Desired.RootTracks.Num() == 1 && Desired.RootTracks[0].Sections.Num() == 1 && Desired.RootTracks[0].Sections[0].NativeChannels.IsValid())) return false;
  Desired.DisplayRate.Numerator = TEXT("24");
  if (!TestTrue(TEXT("unrelated settings edit preserves native channel"), TryUpdateMovieSceneValues(*Scene, Current, Desired, Error))) { AddError(Error.Code + TEXT(":") + Error.Detail); return false; }
  TestEqual(TEXT("requested display rate applied"), Scene->GetDisplayRate().Numerator, 24);
  TestEqual(TEXT("event key retained"), Section->EventChannel.GetNumKeys(), 1);
  TestEqual(TEXT("event key time retained"), Section->EventChannel.GetData().GetTimes()[0].Value, 12);
  return !HasAnyErrors();
}

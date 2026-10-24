#include "Domains/Animation/Authoring/UeAgentAnimationPoseStructure.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringJsonFields.h"

#include "Animation/PoseAsset.h"
#include "Animation/Skeleton.h"
#include "Composition/HjsonImport/UeAgentReflectedObjectRecord.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"
#include "Utilities/AuthoringObjectApply/UeAgentAuthoringObjectApply.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/UnrealType.h"

namespace UeAgentAnimationAuthoring {
namespace {
UE::UAI::Contract::FDiagnostic NativeFailure(const FString& Code,
    const FString& Reason, const UPoseAsset& Asset) {
  auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("pose_validation"));
  Diagnostic.SourceKind = TEXT("native_result");
  Diagnostic.InternalContext = Asset.GetPathName();
  return Diagnostic;
}

void BindPoseInput(UE::UAI::Contract::FDiagnostic& Diagnostic,
    const TSharedPtr<FJsonObject>& Properties, const TArray<FString>& Paths) {
  if (!Paths.IsEmpty()) Diagnostic.PropertyPath = Paths[0];
  if (!Properties) return;
  Diagnostic.SourceKind.Reset();
  const TSharedPtr<FJsonValue> Value = MakeShared<FJsonValueObject>(Properties);
  for (const auto& Path : Paths) {
    Diagnostic.PropertyPath = Path;
    UeAgentDetailsAuthoringService::LocateReadablePropertyFailure(Diagnostic, Value, TEXT("/pose_data"));
    if (!Diagnostic.Source.JsonPath.IsEmpty() && Diagnostic.Source.JsonPath != TEXT("/pose_data")) {
      Diagnostic.SourceKind = TEXT("related_input");
      return;
    }
  }
  Diagnostic.PropertyPath = Paths.IsEmpty() ? FString() : Paths[0];
  Diagnostic.Source.JsonPath.Reset();
  Diagnostic.Source.InternalJsonPath.Reset();
  Diagnostic.SourceKind = TEXT("native_result");
}

UeAgentDetailsAuthoringService::FCapturePolicy PosePolicy() {
  UeAgentDetailsAuthoringService::FCapturePolicy Policy;
  Policy.AuthorizeProperty = [](const FProperty* Property) {
    const UStruct* Owner = Property->GetOwnerStruct();
    const FName Name = Property->GetFName();
    if (Owner == FPoseDataContainer::StaticStruct())
      return Name == TEXT("PoseFNames") || Name == TEXT("Tracks") || Name == TEXT("Poses") || Name == TEXT("Curves");
    if (Owner == FPoseData::StaticStruct())
      return Name == TEXT("SourceLocalSpacePose") || Name == TEXT("SourceCurveData");
    if (Owner == FAnimCurveBase::StaticStruct())
      return Name == TEXT("CurveName") || Name == TEXT("CurveTypeFlags");
    return false;
  };
  Policy.StructFieldFilter = [Authorize = Policy.AuthorizeProperty](const UScriptStruct* Type, const FProperty* Field) {
    if (Type == FPoseData::StaticStruct() || Type == FAnimCurveBase::StaticStruct()) return Authorize(Field);
    return true;
  };
  return Policy;
}

FPoseDataContainer* Container(UPoseAsset& Asset) {
  auto* Property = FindFProperty<FStructProperty>(UPoseAsset::StaticClass(), TEXT("PoseContainer"));
  return Property ? Property->ContainerPtrToValuePtr<FPoseDataContainer>(&Asset) : nullptr;
}

bool ValidateSource(UPoseAsset& Asset, FPoseDataContainer& Data,
    const TSharedPtr<FJsonObject>& Input, UE::UAI::Contract::FDiagnostic& Error) {
  const auto* NamesProperty = FindFProperty<FArrayProperty>(FPoseDataContainer::StaticStruct(), TEXT("PoseFNames"));
  const auto* TracksProperty = FindFProperty<FArrayProperty>(FPoseDataContainer::StaticStruct(), TEXT("Tracks"));
  const auto* PosesProperty = FindFProperty<FArrayProperty>(FPoseDataContainer::StaticStruct(), TEXT("Poses"));
  const auto* CurvesProperty = FindFProperty<FArrayProperty>(FPoseDataContainer::StaticStruct(), TEXT("Curves"));
  if (!NamesProperty || !TracksProperty || !PosesProperty || !CurvesProperty || !Asset.GetSkeleton()) {
    Error = NativeFailure(TEXT("pose_source_layout_or_skeleton_missing"),
        TEXT("The pose source is missing required container fields or its skeleton."), Asset);
    const auto Actual = MakeShared<FJsonObject>();
    Actual->SetBoolField(TEXT("pose_names"), NamesProperty != nullptr);
    Actual->SetBoolField(TEXT("tracks"), TracksProperty != nullptr);
    Actual->SetBoolField(TEXT("poses"), PosesProperty != nullptr);
    Actual->SetBoolField(TEXT("curves"), CurvesProperty != nullptr);
    Actual->SetBoolField(TEXT("skeleton"), Asset.GetSkeleton() != nullptr);
    const auto Expected = MakeShared<FJsonObject>();
    for (const auto& Field : Actual->Values) Expected->SetBoolField(Field.Key, true);
    Error.ActualValue = MakeShared<FJsonValueObject>(Actual);
    Error.ExpectedValue = MakeShared<FJsonValueObject>(Expected);
    return false;
  }
  const auto& Names = *NamesProperty->ContainerPtrToValuePtr<TArray<FName>>(&Data);
  const auto& Tracks = *TracksProperty->ContainerPtrToValuePtr<TArray<FName>>(&Data);
  const auto& Poses = *PosesProperty->ContainerPtrToValuePtr<TArray<FPoseData>>(&Data);
  const auto& Curves = *CurvesProperty->ContainerPtrToValuePtr<TArray<FAnimCurveBase>>(&Data);
  if (Names.Num() != Poses.Num()) {
    Error = NativeFailure(TEXT("pose_name_data_count_mismatch"), TEXT("The pose name count differs from the pose data count."), Asset);
    Error.ActualValue = MakeShared<FJsonValueNumber>(Names.Num());
    Error.ExpectedValue = MakeShared<FJsonValueNumber>(Poses.Num());
    BindPoseInput(Error, Input, {TEXT("PoseFNames"), TEXT("Poses")});
    return false;
  }
  TSet<FName> Seen;
  for (FName Name : Names) {
    if (Name.IsNone() || Seen.Contains(Name)) {
      Error = NativeFailure(TEXT("pose_name_empty_or_duplicate"),
          Name.IsNone() ? TEXT("A pose name resolves to None.") : TEXT("A pose name duplicates an earlier name."), Asset);
      Error.ActualValue = MakeShared<FJsonValueString>(Name.ToString());
      Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-None unique pose name"));
      BindPoseInput(Error, Input, {FString::Printf(TEXT("PoseFNames[%d]"), Seen.Num())});
      return false;
    }
    Seen.Add(Name);
  }
  Seen.Reset();
  for (FName Track : Tracks) {
    if (Seen.Contains(Track) || Asset.GetSkeleton()->GetReferenceSkeleton().FindBoneIndex(Track) == INDEX_NONE) {
      Error = NativeFailure(TEXT("pose_track_unknown_or_duplicate"),
          Seen.Contains(Track) ? TEXT("A pose track duplicates an earlier track.")
              : TEXT("The pose track is not a bone in the asset's skeleton."), Asset);
      Error.ActualValue = MakeShared<FJsonValueString>(Track.ToString());
      Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("unique bone name from the asset skeleton"));
      BindPoseInput(Error, Input, {FString::Printf(TEXT("Tracks[%d]"), Seen.Num())});
      return false;
    }
    Seen.Add(Track);
  }
  Seen.Reset();
  for (const auto& Curve : Curves) {
    if (Curve.GetName().IsNone() || Seen.Contains(Curve.GetName())) {
      Error = NativeFailure(TEXT("pose_curve_empty_or_duplicate"), Curve.GetName().IsNone()
          ? TEXT("A pose curve name resolves to None.") : TEXT("A pose curve name duplicates an earlier curve."), Asset);
      Error.ActualValue = MakeShared<FJsonValueString>(Curve.GetName().ToString());
      Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-None unique curve name"));
      BindPoseInput(Error, Input, {FString::Printf(TEXT("Curves[%d].CurveName"), Seen.Num())});
      return false;
    }
    Seen.Add(Curve.GetName());
  }
  for (int32 PoseIndex = 0; PoseIndex < Poses.Num(); ++PoseIndex) {
    const auto& Pose = Poses[PoseIndex];
    if (Pose.SourceLocalSpacePose.Num() != Tracks.Num() || Pose.SourceCurveData.Num() != Curves.Num()) {
      Error = NativeFailure(TEXT("pose_source_track_curve_count_mismatch"),
          TEXT("The pose sample counts differ from the container's track or curve counts."), Asset);
      Error.SubjectId = Names[PoseIndex].ToString();
      const auto Actual = MakeShared<FJsonObject>();
      Actual->SetNumberField(TEXT("tracks"), Pose.SourceLocalSpacePose.Num());
      Actual->SetNumberField(TEXT("curves"), Pose.SourceCurveData.Num());
      const auto Expected = MakeShared<FJsonObject>();
      Expected->SetNumberField(TEXT("tracks"), Tracks.Num());
      Expected->SetNumberField(TEXT("curves"), Curves.Num());
      Error.ActualValue = MakeShared<FJsonValueObject>(Actual);
      Error.ExpectedValue = MakeShared<FJsonValueObject>(Expected);
      const bool bTracksMismatch = Pose.SourceLocalSpacePose.Num() != Tracks.Num();
      BindPoseInput(Error, Input, {FString::Printf(TEXT("Poses[%d].%s"), PoseIndex,
          bTracksMismatch ? TEXT("SourceLocalSpacePose") : TEXT("SourceCurveData")),
          bTracksMismatch ? TEXT("Tracks") : TEXT("Curves")});
      return false;
    }
  }
  return true;
}
}

TSharedPtr<FJsonObject> ReadPoseStructure(UPoseAsset& Asset, UE::UAI::Contract::FDiagnostic& Error) {
  auto* Data = Container(Asset);
  if (!Data) {
    Error = NativeFailure(TEXT("pose_container_missing"), TEXT("The pose asset has no reflected pose data container to read."), Asset);
    Error.ActualValue = MakeShared<FJsonValueNull>();
    Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("FPoseDataContainer"));
    return nullptr;
  }
  TSharedPtr<FJsonObject> Properties, Definitions;
  if (!UE::UAI::Composition::HjsonImport::CaptureReflectedStructRecord(*FPoseDataContainer::StaticStruct(), Data, Asset,
      {TEXT("PoseFNames"), TEXT("Tracks"), TEXT("Poses"), TEXT("Curves")}, PosePolicy(), Properties, Definitions, Error)) return nullptr;
  auto Document = MakeShared<FJsonObject>();
  Document->SetObjectField(TEXT("pose_data"), Properties);
  Document->SetObjectField(TEXT("pose_data_definitions"), Definitions);
  auto Settings = MakeShared<FJsonObject>();
  Settings->SetBoolField(TEXT("additive"), Asset.IsValidAdditive());
  Settings->SetStringField(TEXT("base_pose"), Asset.GetPoseNameByIndex(Asset.GetBasePoseIndex()).ToString());
  Document->SetObjectField(TEXT("settings"), Settings);
  return Document;
}

bool ApplyPoseStructure(UPoseAsset& Asset, const TSharedPtr<const FJsonObject>& Document, UE::UAI::Contract::FDiagnostic& Error,
    const FString& CanonicalAssetPath) {
  if (!Document) return true;
  if (!UeAgentAuthoringJsonFields::ValidateOptionalFields(*Document,
      {{TEXT("settings"), EJson::Object}}, Error)) return false;
  const TSharedPtr<FJsonObject>* Settings = nullptr;
  if (Document->TryGetObjectField(TEXT("settings"), Settings)
      && !UeAgentAuthoringJsonFields::ValidateOptionalFields(**Settings,
          {{TEXT("additive"), EJson::Boolean}, {TEXT("base_pose"), EJson::String}}, Error, TEXT("/settings"))) return false;
  auto* Data = Container(Asset);
  if (!Data) {
    Error = NativeFailure(TEXT("pose_container_missing"), TEXT("The pose asset has no reflected pose data container to edit."), Asset);
    Error.ActualValue = MakeShared<FJsonValueNull>();
    Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("FPoseDataContainer"));
    return false;
  }
  bool Additive = Asset.IsValidAdditive();
  FString BaseName = Asset.GetPoseNameByIndex(Asset.GetBasePoseIndex()).ToString();
  const TSharedPtr<FJsonObject>* Properties = nullptr;
  if (Document->HasField(TEXT("pose_data"))) {
    if (!Document->TryGetObjectField(TEXT("pose_data"), Properties)) {
      Error = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("pose_data_expected_object"), TEXT("/pose_data"),
          MakeShared<FJsonValueString>(TEXT("object")), Document->TryGetField(TEXT("pose_data")), TEXT("pose_apply"),
          TEXT("pose_data must be a reflected pose properties object."));
      return false;
    }
    auto Policy = PosePolicy();
    UeAgentAuthoringObjectApply::FOptions Options;
    Options.DiagnosticTargetPath = CanonicalAssetPath;
    Options.bAllowDomainAuthorizedOwners = true;
    Options.AuthorizeProperty = Policy.AuthorizeProperty;
    Options.bEmitEditorNotifications = false;
    UeAgentAuthoringObjectApply::FPreparedApply Prepared;
    if (!UE::UAI::Composition::HjsonImport::PrepareReflectedStructRecord(*FPoseDataContainer::StaticStruct(), Data, Asset,
        {TEXT("PoseFNames"), TEXT("Tracks"), TEXT("Poses"), TEXT("Curves")}, Policy, *Properties, Options, Prepared, Error)) {
      UeAgentDetailsAuthoringService::LocateReadablePropertyFailure(Error,
          MakeShared<FJsonValueObject>(*Properties), TEXT("/pose_data"));
      return false;
    }
    UeAgentAuthoringObjectApply::FApplyResult Result;
    if (Prepared.IsReady() && (!UeAgentAuthoringObjectApply::ApplyPrepared(Prepared, Result))) {
      Error = Result.Diagnostic;
      UeAgentDetailsAuthoringService::LocateReadablePropertyFailure(Error,
          MakeShared<FJsonValueObject>(*Properties), TEXT("/pose_data")); return false;
    }
  }
  if (!ValidateSource(Asset, *Data, Properties ? *Properties : TSharedPtr<FJsonObject>(), Error)) return false;
  if (Document->TryGetObjectField(TEXT("settings"), Settings)) {
    (*Settings)->TryGetBoolField(TEXT("additive"), Additive);
    (*Settings)->TryGetStringField(TEXT("base_pose"), BaseName);
  }
  const int32 BaseIndex = BaseName.IsEmpty() || BaseName == TEXT("None") ? INDEX_NONE : Asset.GetPoseIndexByName(FName(*BaseName));
  if (BaseIndex == INDEX_NONE && !BaseName.IsEmpty() && BaseName != TEXT("None")) {
    Error = NativeFailure(TEXT("pose_base_pose_not_found"), TEXT("The selected base pose does not exist in the pose asset."), Asset);
    Error.ActualValue = MakeShared<FJsonValueString>(BaseName);
    Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("existing pose name"));
    if (Settings && (*Settings)->HasField(TEXT("base_pose"))) {
      Error.SourceKind = TEXT("related_input");
      Error.Source.JsonPath = TEXT("/settings/base_pose");
      Error.PropertyPath = TEXT("base_pose");
    } else BindPoseInput(Error, Properties ? *Properties : TSharedPtr<FJsonObject>(), {TEXT("PoseFNames")});
    return false;
  }
  return Asset.ConvertSpace(Additive, BaseIndex);
}
}

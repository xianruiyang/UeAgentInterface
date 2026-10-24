// Copyright Epic Games, Inc. All Rights Reserved.
#include "Utilities/AuthoringObjectApply/UeAgentPrimitiveComponentNormalization.h"

#include "Utilities/AuthoringObjectApply/UeAgentAuthoringObjectApply.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Components/PrimitiveComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/CollisionProfile.h"
#include "PhysicsEngine/BodyInstance.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

namespace UeAgentAuthoringObjectApply {
bool NormalizePrimitiveComponentProperties(UObject* Proposed, UObject* Baseline,
    TArray<TSharedPtr<FJsonValue>>& Descriptors, const FOptions& Options,
    FFailure& OutFailure) {
  auto* Primitive = Cast<UPrimitiveComponent>(Proposed);
  if (!Primitive) return true;
  const int32 BodyIndex = Descriptors.IndexOfByPredicate([](const auto& Value) {
    FString Path;
    return Value.IsValid() && Value->Type == EJson::Object
        && Value->AsObject()->TryGetStringField(TEXT("property_path"), Path)
        && Path == TEXT("BodyInstance");
  });
  if (BodyIndex == INDEX_NONE) return true;
  const TSharedPtr<FJsonObject>* Fields = nullptr;
  const auto Value = Descriptors[BodyIndex]->AsObject()->TryGetField(TEXT("value"));
  if (Value.IsValid() && Value->Type == EJson::Object)
    Value->AsObject()->TryGetObjectField(TEXT("fields"), Fields);
  const bool bProfileAuthored = Fields && (*Fields)->HasField(TEXT("CollisionProfileName"));
  const bool bTypeAuthored = Fields && (*Fields)->HasField(TEXT("ObjectType"));
  const bool bEnabledAuthored = Fields && (*Fields)->HasField(TEXT("CollisionEnabled"));
  const TSharedPtr<FJsonObject>* ResponseFields = nullptr;
  const auto ResponseValue = Fields ? (*Fields)->TryGetField(TEXT("CollisionResponses")) : nullptr;
  const bool bResponsesAuthored = ResponseValue && ResponseValue->Type == EJson::Object
      && ResponseValue->AsObject()->TryGetObjectField(TEXT("fields"), ResponseFields)
      && (*ResponseFields)->HasField(TEXT("ResponseArray"));
  if (Fields && !bProfileAuthored && !bTypeAuthored && !bEnabledAuthored && !bResponsesAuthored)
    return true;

  const auto* Existing = Cast<UPrimitiveComponent>(Baseline);
  const FBodyInstance& BaselineBody = Existing ? Existing->BodyInstance : Primitive->BodyInstance;
  TStrongObjectPtr<UPrimitiveComponent> PreviewObject(
      DuplicateObject<UPrimitiveComponent>(Primitive, GetTransientPackage()));
  if (!PreviewObject.IsValid()) {
    OutFailure.Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("component_normalization_preview_failed"), TEXT("The component preview could not be created."), TEXT("component_normalization"));
    OutFailure.Diagnostic.PropertyPath = TEXT("BodyInstance");
    return false;
  }
  FBodyInstance& Body = PreviewObject->BodyInstance;
  // Copy reflected state only; never copy a live physics body's runtime handles.
  for (TFieldIterator<FProperty> It(FBodyInstance::StaticStruct()); It; ++It)
    It->CopyCompleteValue(It->ContainerPtrToValuePtr<void>(&Body),
        It->ContainerPtrToValuePtr<void>(&BaselineBody));
  const FProperty* ResponsesProperty = FindFProperty<FProperty>(
      FBodyInstance::StaticStruct(), TEXT("CollisionResponses"));

  auto PreviewOptions = Options;
  PreviewOptions.bEmitEditorNotifications = false;
  PreviewOptions.bMarkPackageDirty = false;
  PreviewOptions.NativeStages.Reset();
  FPreparedApply Preview;
  FApplyResult Result;
  TArray<TSharedPtr<FJsonValue>> BodyDescriptors{Descriptors[BodyIndex]};
  if (!PrepareObject(PreviewObject.Get(), BodyDescriptors, PreviewOptions, Preview, OutFailure))
    return false;
  if (!Preview.ApplyNativeNormalizationPreview(Result)) {
    OutFailure.Diagnostic = MoveTemp(Result.Diagnostic);
    return false;
  }
  if (Preview.NumProperties() == 0) return true;
  // A workspace contains unchanged exported members as well as edits. Compare
  // the decoded native members with the real baseline before loading a profile;
  // presence alone must not turn its old defaults into custom overrides.
  UeAgentPropertyValueCodec::FOptions CompareOptions;
  CompareOptions.bCompareStoredValues = true;
  const auto MemberChanged = [&](const FName Name) {
    FProperty* Property = FindFProperty<FProperty>(FBodyInstance::StaticStruct(), Name);
    return !UeAgentPropertyValueCodec::AreSemanticallyEquivalent(Property,
        Property->ContainerPtrToValuePtr<void>(&Body),
        Property->ContainerPtrToValuePtr<void>(&BaselineBody),
        PreviewObject.Get(), CompareOptions);
  };
  const bool bTypeChanged = bTypeAuthored && MemberChanged(TEXT("ObjectType"));
  const bool bEnabledChanged = bEnabledAuthored && MemberChanged(TEXT("CollisionEnabled"));
  auto* Responses = static_cast<FCollisionResponse*>(
      ResponsesProperty->ContainerPtrToValuePtr<void>(&Body));
  Responses->UpdateResponseContainerFromArray();
  FArrayProperty* ResponseArrayProperty = FindFProperty<FArrayProperty>(
      FCollisionResponse::StaticStruct(), TEXT("ResponseArray"));
  const void* BaselineResponseStorage = ResponsesProperty->ContainerPtrToValuePtr<void>(&BaselineBody);
  const bool bResponsesChanged = bResponsesAuthored && !UeAgentPropertyValueCodec::AreSemanticallyEquivalent(
      ResponseArrayProperty, ResponseArrayProperty->ContainerPtrToValuePtr<void>(Responses),
      ResponseArrayProperty->ContainerPtrToValuePtr<void>(BaselineResponseStorage), PreviewObject.Get(), CompareOptions);
  const TArray<FResponseChannel> ExplicitResponses =
      *ResponseArrayProperty->ContainerPtrToValuePtr<TArray<FResponseChannel>>(Responses);
  const FCollisionResponseContainer DesiredResponses = Body.GetResponseToChannels();
  // A reflected profile edit precedes clearing a native external-profile cache.
  const FName DesiredProfile = bProfileAuthored
      ? FindFProperty<FNameProperty>(FBodyInstance::StaticStruct(), TEXT("CollisionProfileName"))
          ->GetPropertyValue_InContainer(&Body)
      : Body.GetCollisionProfileName();
  const auto DesiredType = Body.GetObjectType();
  const auto DesiredEnabled = Body.GetCollisionEnabled(false);
  const bool bProfileChanged = bProfileAuthored && DesiredProfile != BaselineBody.GetCollisionProfileName();
  if (bProfileAuthored) Body.SetCollisionProfileName(DesiredProfile);
  Body.LoadProfileData(false);
  Responses->UpdateResponseContainerFromArray();
  if (bTypeChanged && DesiredType != Body.GetObjectType())
    Body.SetObjectType(DesiredType);
  if (bEnabledChanged && DesiredEnabled != Body.GetCollisionEnabled(false))
    Body.SetCollisionEnabled(DesiredEnabled);
  if (bResponsesChanged) {
    for (int32 Index = 0; Index < UE_ARRAY_COUNT(DesiredResponses.EnumArray); ++Index) {
      const auto Channel = static_cast<ECollisionChannel>(Index);
      // A changed list is authored as a collection, including retained entries
      // whose value equals the old profile. When changing the profile as well,
      // its defaults own omitted channels; unchanged exported lists own none.
      const FName ChannelName = UCollisionProfile::Get()->ReturnChannelNameFromContainerIndex(Index);
      const bool bExplicit = ExplicitResponses.ContainsByPredicate([&](const FResponseChannel& Response) {
        return Response.Channel == ChannelName;
      });
      if ((!bProfileChanged || bExplicit)
          && DesiredResponses.GetResponse(Channel) != Body.GetResponseToChannels().GetResponse(Channel))
        Body.SetResponseToChannel(Channel, DesiredResponses.GetResponse(Channel));
    }
  }
  // Native profile setters skip rebuilding the array when their cached response
  // container already matches. A reflected edit can therefore leave redundant
  // default entries in this preview while the actual native update removes them.
  // Rebuild from UE's empty-array baseline, then let its container setter produce
  // canonical editable storage, including project-defined channel defaults.
  Responses->UpdateResponseContainerFromArray();
  FCollisionResponse NormalizedResponses;
  NormalizedResponses.SetResponsesArray({});
  NormalizedResponses.SetCollisionResponseContainer(Responses->GetResponseContainer());
  *Responses = MoveTemp(NormalizedResponses);
  if (!Preview.CaptureNativeNormalizedDescriptors(BodyDescriptors, OutFailure)) return false;
  Descriptors[BodyIndex] = BodyDescriptors[0];
  return true;
}
}

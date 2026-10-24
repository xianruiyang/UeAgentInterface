#include "Domains/Level/Authoring/UeAgentLevelAuthoringDescriptors.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringState.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticCompare.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Components/SceneComponent.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUeAgentLevelActorCandidateSemanticVerificationTest,
    "UeAgentInterface.DomainAdapters.WorldSpatial.E1.LevelActorCandidateSemanticVerification",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentLevelActorCandidateSemanticVerificationTest::RunTest(
    const FString &Parameters) {
  FString Error;
  if (!TestTrue(TEXT("Level descriptors are registered"),
                UeAgentLevelAuthoring::RegisterDescriptors(Error))) {
    AddError(TEXT("Level descriptor error: ") + Error);
    return false;
  }
  if (!TestTrue(TEXT("Level state accessor is registered"),
                UeAgentLevelAuthoring::RegisterStateAccessor(Error))) {
    return false;
  }

  const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
  UPackage *Package =
      CreatePackage(*FString::Printf(TEXT("/Temp/UAI/WorldSpatial/%s"),
                                    *Suffix));
  if (!TestNotNull(TEXT("Owned package is available"), Package)) {
    return false;
  }
  UWorld *World = UWorld::CreateWorld(EWorldType::EditorPreview, false,
                                     FName(TEXT("LevelActorSemanticWorld")),
                                     Package);
  TestNotNull(TEXT("Owned preview world is available"), World);
  if (!World)
    return false;

  AActor *ExpectedActor = World->SpawnActor<AStaticMeshActor>();
  AActor *FactoryActor = World->SpawnActor<AStaticMeshActor>();
  TestNotNull(TEXT("Expected actor is available"), ExpectedActor);
  TestNotNull(TEXT("Factory actor is available"), FactoryActor);
  if (!ExpectedActor || !FactoryActor) {
    World->DestroyWorld(false);
    return false;
  }
  ExpectedActor->Tags.AddUnique(
      UeAgentLevelAuthoring::GetManagedActorMarkerTag());
  ExpectedActor->Tags.AddUnique(FName(*UeAgentLevelAuthoring::MakeManagedActorTag(
      TEXT("SemanticExpected"))));
  FactoryActor->Tags.AddUnique(
      UeAgentLevelAuthoring::GetManagedActorMarkerTag());
  FactoryActor->Tags.AddUnique(FName(*UeAgentLevelAuthoring::MakeManagedActorTag(
      TEXT("SemanticFactory"))));

  ExpectedActor->SetActorTransform(FTransform(
      FRotator(-45.0, -30.0, 0.0), FVector(0.0, 0.0, 500.0),
      FVector::OneVector));
  FactoryActor->SetActorTransform(FTransform(
      FRotator(-45.0, -30.0, 0.0), FVector(0.0, 0.0, 500.0),
      FVector(2.0, 3.0, 4.0)));

  UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
  if (!TestTrue(TEXT("Actor identity builds"),
                UeAgentLevelAuthoring::BuildActorIdentity(
                    *ExpectedActor, Identity, Error))) {
    AddError(TEXT("Actor identity error: ") + Error);
    World->DestroyWorld(false);
    return false;
  }

  auto Capture = [&](AActor *Actor, const bool bOmitScale,
                     UeAgentAuthoringState::FStateSnapshot &OutSnapshot) {
    TArray<UeAgentAuthoringState::FReadRequest> Requests;
    if (!UeAgentLevelAuthoring::BuildActorSnapshotReadRequests(
            Actor, Identity, false, Requests, Error) || Requests.Num() != 1) {
      return false;
    }
    Requests[0].Context.Add(TEXT("actor_label_omit"), TEXT("true"));
    if (bOmitScale) {
      Requests[0].Context.Add(TEXT("actor_transform_scale_omit"),
                              TEXT("true"));
    }
    UeAgentAuthoringState::FCaptureResult CaptureResult;
    if (!UeAgentAuthoringState::CaptureSnapshot(Requests, CaptureResult) ||
        !CaptureResult.IsCaptured()) {
      return false;
    }
    OutSnapshot = MoveTemp(CaptureResult.Snapshot);
    return true;
  };

  UeAgentAuthoringState::FStateSnapshot ExpectedProjected;
  UeAgentAuthoringState::FStateSnapshot ActualProjected;
  if (!TestTrue(TEXT("Expected projected actor state captures"),
                Capture(ExpectedActor, true, ExpectedProjected)) ||
      !TestTrue(TEXT("Factory projected actor state captures"),
                Capture(FactoryActor, true, ActualProjected))) {
    World->DestroyWorld(false);
    return false;
  }
  UeAgentAuthoringSemanticCompare::FComparisonReport ProjectedReport;
  if (!TestTrue(TEXT("Projected actor states compare"),
                UeAgentAuthoringSemanticCompare::CompareSnapshots(
                    ExpectedProjected, ActualProjected,
                    UeAgentAuthoringSemanticCompare::FCompareOptions(),
                    ProjectedReport))) {
    World->DestroyWorld(false);
    return false;
  }
  TestTrue(TEXT("Omitted factory-owned scale is not a semantic mismatch"),
           ProjectedReport.bEquivalent);

  UeAgentAuthoringState::FStateSnapshot ExpectedFull;
  UeAgentAuthoringState::FStateSnapshot ActualFull;
  if (!TestTrue(TEXT("Expected full actor state captures"),
                Capture(ExpectedActor, false, ExpectedFull)) ||
      !TestTrue(TEXT("Factory full actor state captures"),
                Capture(FactoryActor, false, ActualFull))) {
    World->DestroyWorld(false);
    return false;
  }
  UeAgentAuthoringSemanticCompare::FComparisonReport FullReport;
  if (!TestTrue(TEXT("Full actor states compare"),
                UeAgentAuthoringSemanticCompare::CompareSnapshots(
                    ExpectedFull, ActualFull,
                    UeAgentAuthoringSemanticCompare::FCompareOptions(),
                    FullReport))) {
    World->DestroyWorld(false);
    return false;
  }
  TestFalse(TEXT("Explicitly compared scale remains protected"),
            FullReport.bEquivalent);

  const TArray<UeAgentAuthoringState::FStateEntry> FullEntries =
      ExpectedFull.CopyEntries();
  if (FullEntries.Num() == 1) {
    auto SpatialEntry = [&](double Pitch) {
      auto Entry = FullEntries[0];
      Entry.Value = UeAgentAuthoringState::CloneCanonicalValue(Entry.Value);
      auto Transform = Entry.Value->AsObject()->GetObjectField(TEXT("transform"));
      Transform->GetObjectField(TEXT("rotation"))->SetArrayField(TEXT("components"), {
          MakeShared<FJsonValueNumber>(Pitch), MakeShared<FJsonValueNumber>(0.0), MakeShared<FJsonValueNumber>(0.0)});
      return Entry;
    };
    auto Desired = SpatialEntry(8.0);
    auto Readback = SpatialEntry(8.000000000000004);
    UeAgentAuthoringSemanticCompare::FEntryComparison SpatialReport;
    TestTrue(TEXT("Nested declared rotation compares"), UeAgentAuthoringSemanticCompare::CompareEntries(
        Desired, Readback, UeAgentAuthoringSemanticCompare::FCompareOptions(), SpatialReport));
    TestTrue(TEXT("Native rotation roundoff is equivalent"), SpatialReport.bEquivalent);
    Readback = SpatialEntry(368.0);
    TestTrue(TEXT("Nested winding compares"), UeAgentAuthoringSemanticCompare::CompareEntries(
        Desired, Readback, UeAgentAuthoringSemanticCompare::FCompareOptions(), SpatialReport));
    TestTrue(TEXT("Equivalent winding is accepted"), SpatialReport.bEquivalent);
    Readback = SpatialEntry(8.01);
    TestTrue(TEXT("Different rotation compares"), UeAgentAuthoringSemanticCompare::CompareEntries(
        Desired, Readback, UeAgentAuthoringSemanticCompare::FCompareOptions(), SpatialReport));
    TestFalse(TEXT("Different rotation remains a failure"), SpatialReport.bEquivalent);

    Readback = SpatialEntry(8.000000000000004);
    Desired.Value->AsObject()->SetNumberField(TEXT("exact_counter"), 8.0);
    Readback.Value->AsObject()->SetNumberField(TEXT("exact_counter"), 8.000000000000004);
    TestTrue(TEXT("Undeclared numeric fields compare"), UeAgentAuthoringSemanticCompare::CompareEntries(
        Desired, Readback, UeAgentAuthoringSemanticCompare::FCompareOptions(), SpatialReport));
    TestFalse(TEXT("Undeclared numbers retain exact semantics"), SpatialReport.bEquivalent);
    TestEqual(TEXT("Diagnostic skips equivalent rotation and identifies the real difference"),
        SpatialReport.ValueDifferencePath, FString(TEXT("/exact_counter")));
    Desired.Value->AsObject()->RemoveField(TEXT("exact_counter"));
    Readback.Value->AsObject()->RemoveField(TEXT("exact_counter"));
    Readback.Value->AsObject()->GetObjectField(TEXT("transform"))->GetObjectField(TEXT("rotation"))
        ->SetStringField(TEXT("frame"), TEXT("different_frame"));
    TestTrue(TEXT("Spatial metadata is compared"), UeAgentAuthoringSemanticCompare::CompareEntries(
        Desired, Readback, UeAgentAuthoringSemanticCompare::FCompareOptions(), SpatialReport));
    TestFalse(TEXT("Spatial comparison does not discard frame metadata"), SpatialReport.bEquivalent);

    Readback = SpatialEntry(8.000000000000004);
    auto UntypedDesired = Desired;
    auto UntypedActual = Readback;
    UntypedDesired.ValueDefinition.Reset();
    UntypedActual.ValueDefinition.Reset();
    TestTrue(TEXT("Untyped object compares"), UeAgentAuthoringSemanticCompare::CompareEntries(
        UntypedDesired, UntypedActual, UeAgentAuthoringSemanticCompare::FCompareOptions(), SpatialReport));
    TestFalse(TEXT("A field named rotation does not imply a declared spatial type"), SpatialReport.bEquivalent);

    // Container declarations retain the element's semantics without relaxing key identity.
    auto ArrayDesired = Desired;
    auto ArrayActual = Readback;
    ArrayDesired.Type.Kind = ArrayActual.Type.Kind = TEXT("array");
    ArrayDesired.Type.ElementType = MakeShared<UeAgentAuthoringTypeDescriptors::FDescriptor>();
    ArrayDesired.Type.ElementType->Kind = TEXT("rotator");
    ArrayActual.Type.ElementType = ArrayDesired.Type.ElementType;
    ArrayDesired.ValueDefinition.Reset();
    ArrayActual.ValueDefinition.Reset();
    const auto Rotation = [](const UeAgentAuthoringState::FStateEntry& Entry) {
      return Entry.Value->AsObject()->GetObjectField(TEXT("transform"))->TryGetField(TEXT("rotation"));
    };
    ArrayDesired.Value = MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{Rotation(Desired)});
    ArrayActual.Value = MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{Rotation(Readback)});
    TestTrue(TEXT("Declared array compares"), UeAgentAuthoringSemanticCompare::CompareEntries(
        ArrayDesired, ArrayActual, UeAgentAuthoringSemanticCompare::FCompareOptions(), SpatialReport));
    TestTrue(TEXT("Array elements use declared rotation semantics"), SpatialReport.bEquivalent);
    ArrayDesired.Type.Kind = ArrayActual.Type.Kind = TEXT("set");
    TestTrue(TEXT("Declared set compares"), UeAgentAuthoringSemanticCompare::CompareEntries(
        ArrayDesired, ArrayActual, UeAgentAuthoringSemanticCompare::FCompareOptions(), SpatialReport));
    TestFalse(TEXT("Tolerance does not merge distinct set identities"), SpatialReport.bEquivalent);

    UeAgentAuthoringState::FStateEntry NumberStringEntry = FullEntries[0];
    UeAgentAuthoringState::FStateEntry NumberEntry = FullEntries[0];
    TSharedRef<FJsonObject> NumberStringObject = MakeShared<FJsonObject>();
    NumberStringObject->SetField(
        TEXT("value"), MakeShared<FJsonValueNumberString>(TEXT("1")));
    NumberStringEntry.Value =
        MakeShared<FJsonValueObject>(NumberStringObject);
    TSharedRef<FJsonObject> NumberObject = MakeShared<FJsonObject>();
    NumberObject->SetNumberField(TEXT("value"), 1.0);
    NumberEntry.Value = MakeShared<FJsonValueObject>(NumberObject);
    UeAgentAuthoringSemanticCompare::FEntryComparison NumberReport;
    if (!TestTrue(TEXT("Mixed JSON number representations compare"),
                  UeAgentAuthoringSemanticCompare::CompareEntries(
                      NumberStringEntry, NumberEntry,
                      UeAgentAuthoringSemanticCompare::FCompareOptions(),
                      NumberReport))) {
      World->DestroyWorld(false);
      return false;
    }
    TestTrue(TEXT("NumberString one equals numeric one"),
             NumberReport.bEquivalent);

    const FString EscapedKey = TEXT("a~/b");
    NumberStringObject->Values.Reset();
    NumberObject->Values.Reset();
    NumberStringObject->SetArrayField(
        EscapedKey, {MakeShared<FJsonValueNumber>(1.0)});
    NumberObject->SetArrayField(
        EscapedKey, {MakeShared<FJsonValueNumber>(2.0)});
    UeAgentAuthoringSemanticCompare::FEntryComparison DifferenceReport;
    TestTrue(TEXT("Canonical value differences are reported"),
             UeAgentAuthoringSemanticCompare::CompareEntries(
                 NumberStringEntry, NumberEntry,
                 UeAgentAuthoringSemanticCompare::FCompareOptions(),
                 DifferenceReport));
    TestFalse(TEXT("Different values remain non-equivalent"),
              DifferenceReport.bEquivalent);
    TestEqual(TEXT("Difference pointer escapes tilde and slash"),
              DifferenceReport.ValueDifferencePath,
              FString(TEXT("/a~0~1b/0")));
    TestEqual(TEXT("Structured comparison carries the same pointer"),
              DifferenceReport.ToJsonObject()->GetStringField(TEXT("difference_path")),
              DifferenceReport.ValueDifferencePath);

    NumberObject->SetArrayField(EscapedKey, {});
    TestTrue(TEXT("Array length differences are reported"),
             UeAgentAuthoringSemanticCompare::CompareEntries(
                 NumberStringEntry, NumberEntry,
                 UeAgentAuthoringSemanticCompare::FCompareOptions(),
                 DifferenceReport));
    TestEqual(TEXT("Array length difference points to the array"),
              DifferenceReport.ValueDifferencePath,
              FString(TEXT("/a~0~1b")));
  } else {
    AddError(TEXT("Expected exactly one actor semantic entry"));
  }

  TArray<UeAgentAuthoringState::FReadRequest> ComponentRequests;
  UeAgentAuthoringIdentity::FCanonicalIdentity ComponentIdentity;
  USceneComponent* Component = ExpectedActor->GetRootComponent();
  if (Component && UeAgentLevelAuthoring::BuildComponentIdentity(*Component, ComponentIdentity, Error)
      && UeAgentLevelAuthoring::BuildComponentSnapshotReadRequests(Component, ComponentIdentity,
          false, ComponentRequests, Error)) {
    UeAgentAuthoringState::FCaptureResult Captured;
    TestTrue(TEXT("Component state captures"), UeAgentAuthoringState::CaptureSnapshot(ComponentRequests, Captured));
    const auto Entries = Captured.Snapshot.CopyEntries();
    if (TestEqual(TEXT("One component semantic entry"), Entries.Num(), 1)) {
      auto Expected = Entries[0];
      auto Actual = Entries[0];
      Expected.Value = UeAgentAuthoringState::CloneCanonicalValue(Expected.Value);
      Actual.Value = UeAgentAuthoringState::CloneCanonicalValue(Actual.Value);
      const auto SetPitch = [](auto& Entry, double Pitch) {
        Entry.Value->AsObject()->GetObjectField(TEXT("attachment"))->GetObjectField(TEXT("transform"))
            ->GetObjectField(TEXT("rotation"))->SetArrayField(TEXT("components"), {
              MakeShared<FJsonValueNumber>(Pitch), MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueNumber>(0)});
      };
      SetPitch(Expected, 8);
      SetPitch(Actual, 8.000000000000004);
      UeAgentAuthoringSemanticCompare::FEntryComparison Report;
      TestTrue(TEXT("Component comparison executes"), UeAgentAuthoringSemanticCompare::CompareEntries(
          Expected, Actual, UeAgentAuthoringSemanticCompare::FCompareOptions(), Report));
      TestTrue(TEXT("Component attachment rotation accepts native roundoff"), Report.bEquivalent);
    }
  } else AddError(TEXT("Component fixture capture failed: ") + Error);

  World->DestroyWorld(false);
  return true;
}

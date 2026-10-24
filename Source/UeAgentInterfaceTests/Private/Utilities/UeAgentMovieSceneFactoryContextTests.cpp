#include "Misc/AutomationTest.h"
#include "Utilities/AuthoringCore/UeAgentMovieSceneScalarChannel.h"
#include "Utilities/AuthoringCore/UeAgentMovieSceneAuthoring.h"
#include "MovieScene.h"
#include "Tracks/MovieSceneFadeTrack.h"
#include "Tracks/MovieSceneSlomoTrack.h"
#include "Tracks/MovieSceneVisibilityTrack.h"
#include "MovieScene/Parameters/MovieSceneNiagaraVectorParameterTrack.h"
#include "Sections/MovieSceneVectorSection.h"
#include "Sequencer/MovieSceneControlRigParameterTrack.h"
#include "Sequencer/MovieSceneControlRigParameterSection.h"
#include "Rigs/AdditiveControlRig.h"
#include "Components/SceneComponent.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUaiMovieSceneFactoryContextTest,
    "UeAgentInterface.Common.MovieSceneFactoryContext",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUaiMovieSceneFactoryContextTest::RunTest(const FString&)
{
    using namespace UE::UAI::Authoring::Timeline::ScalarChannel;
    TestFalse(TEXT("uninitialized Niagara vector cannot create a native section"),
        HasRequiredSectionContext(*GetDefault<UMovieSceneNiagaraVectorParameterTrack>()));
    for (int32 Dimensions : {2, 3, 4})
    {
        TStrongObjectPtr<UMovieSceneNiagaraVectorParameterTrack> Source(
            NewObject<UMovieSceneNiagaraVectorParameterTrack>(GetTransientPackage()));
        Source->SetChannelsUsed(Dimensions);
        auto* Existing = Source->CreateNewSection();
        Source->AddSection(*Existing);
        auto* ExistingChannel = Existing->GetChannelProxy().GetChannel<FMovieSceneFloatChannel>(0);
        ExistingChannel->AddCubicKey(10, 37.5f);
        TStrongObjectPtr<UMovieSceneTrack> Template(MakeSectionTemplateTrack(*Source));
        if (!TestNotNull(TEXT("initialized native track duplicates"), Template.Get())) return false;
        auto* Section = Cast<UMovieSceneFloatVectorSection>(Template->CreateNewSection());
        if (!TestNotNull(TEXT("Niagara section factory retains its native context"), Section)) return false;
        TestEqual(TEXT("vector dimensions retained"), Section->GetChannelsUsed(), Dimensions);
        for (int32 Index = 0; Index < Dimensions; ++Index)
            TestEqual(TEXT("fresh section does not inherit old keys"), Section->GetChannel(Index).GetNumKeys(), 0);
        TestEqual(TEXT("source section count unchanged"), Source->GetAllSections().Num(), 1);
        TestEqual(TEXT("source key unchanged"), ExistingChannel->GetData().GetValues()[0].Value, 37.5f);
    }

    TStrongObjectPtr<UMovieSceneObjectPropertyTrack> Object(
        NewObject<UMovieSceneObjectPropertyTrack>(GetTransientPackage()));
    Object->PropertyClass = USceneComponent::StaticClass();
    Object->bClassProperty = true;
    TStrongObjectPtr<UMovieSceneTrack> ObjectTemplate(MakeSectionTemplateTrack(*Object));
    if (!TestNotNull(TEXT("object property factory context duplicates"), ObjectTemplate.Get())) return false;
    auto* ObjectSection = ObjectTemplate->CreateNewSection();
    auto* ObjectChannel = ObjectSection->GetChannelProxy().GetChannel<FMovieSceneObjectPathChannel>(0);
    TestTrue(TEXT("class constraint retained"), ObjectChannel->GetPropertyClass() == USceneComponent::StaticClass());
    TestTrue(TEXT("class mode retained"), CastChecked<UMovieSceneObjectPropertyTrack>(ObjectTemplate.Get())->bClassProperty);
    TestEqual(TEXT("fresh object section has no keys"), ObjectChannel->GetData().GetValues().Num(), 0);

    TStrongObjectPtr<UMovieSceneControlRigParameterTrack> RigTrack(
        NewObject<UMovieSceneControlRigParameterTrack>(GetTransientPackage()));
    TStrongObjectPtr<UMovieSceneTrack> EmptyRigTemplate(MakeSectionTemplateTrack(*RigTrack));
    if (!TestNotNull(TEXT("empty rig template"), EmptyRigTemplate.Get())) return false;
    TestTrue(TEXT("native first section remains absolute"),
        EmptyRigTemplate->CreateNewSection()->GetBlendType().Get() == EMovieSceneBlendType::Absolute);
    auto* Rig = NewObject<UAdditiveControlRig>(RigTrack.Get());
    Rig->Initialize(true);
    auto* First = RigTrack->CreateControlRigSection(0, Rig, true);
    if (!TestNotNull(TEXT("native first rig section"), First)) return false;
    TStrongObjectPtr<UMovieSceneTrack> RigTemplate(MakeSectionTemplateTrack(*RigTrack));
    if (!TestNotNull(TEXT("owned rig template"), RigTemplate.Get())) return false;
    auto* Copy = CastChecked<UMovieSceneControlRigParameterTrack>(RigTemplate.Get());
    auto* Next = Cast<UMovieSceneControlRigParameterSection>(Copy->CreateNewSection());
    if (!TestNotNull(TEXT("native subsequent rig section"), Next)) return false;
    TestTrue(TEXT("subsequent native section is additive"), Next->GetBlendType().Get() == EMovieSceneBlendType::Additive);
    TestNotNull(TEXT("owned rig context retained"), Copy->GetControlRig());
    TestTrue(TEXT("section uses duplicated native owner"), Next->GetControlRig() == Copy->GetControlRig());
    TestTrue(TEXT("source owned rig unchanged"), RigTrack->GetControlRig() == Rig);
    TestEqual(TEXT("source rig section count unchanged"), RigTrack->GetAllSections().Num(), 1);
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUaiMovieScenePropertyCandidateTest,
    "UeAgentInterface.Common.MovieScenePropertyCandidates",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUaiMovieScenePropertyCandidateTest::RunTest(const FString&)
{
    using namespace UE::UAI::Authoring::Timeline;
    TStrongObjectPtr<UMovieScene> Scene(NewObject<UMovieScene>(GetTransientPackage()));
    TStrongObjectPtr<USceneComponent> Component(NewObject<USceneComponent>(GetTransientPackage()));
    FString Payload, Error;
    auto Query = [&](const TCHAR* Path) {
        return TestTrue(Path, TryQueryMovieSceneTrackCandidates(*Scene, {}, Payload, Error, Component.Get(), Path));
    };
    auto Has = [&](UClass* Class) { return Payload.Contains(Class->GetDisplayNameText().ToString()); };
    if (!Query(TEXT("PrimaryComponentTick.TickInterval"))) return false;
    TestTrue(TEXT("float property retains its native track"), Has(UMovieSceneFloatTrack::StaticClass()));
    TestFalse(TEXT("slomo is not a float property animator"), Has(UMovieSceneSlomoTrack::StaticClass()));
    TestFalse(TEXT("fade is not a float property animator"), Has(UMovieSceneFadeTrack::StaticClass()));
    if (!Query(TEXT("bAutoActivate"))) return false;
    TestTrue(TEXT("ordinary bool uses BoolTrack"), Has(UMovieSceneBoolTrack::StaticClass()));
    TestFalse(TEXT("ordinary bool does not inherit visibility semantics"), Has(UMovieSceneVisibilityTrack::StaticClass()));
    if (!Query(TEXT("bHiddenInGame"))) return false;
    TestTrue(TEXT("native metadata selects VisibilityTrack"), Has(UMovieSceneVisibilityTrack::StaticClass()));
    TestFalse(TEXT("metadata override replaces ordinary bool track"), Has(UMovieSceneBoolTrack::StaticClass()));
    if (!Query(TEXT("RelativeLocation"))) return false;
    TestTrue(TEXT("native double-vector property context retained"), Has(UMovieSceneDoubleVectorTrack::StaticClass()));

    Scene->SetPlaybackRange(0, 0);
    auto* Track = Scene->AddTrack<UMovieSceneFloatTrack>();
    TSharedPtr<FJsonObject> Structure;
    FUeAgentTimelineAnimationExecutionError CaptureError;
    if (!TestTrue(TEXT("empty-scene capture"), TryReadMovieSceneStructure(*Scene, TEXT("test"), {}, Structure, CaptureError))) return false;
    const auto Captured = Structure->GetArrayField(TEXT("root_tracks"))[0]->AsObject();
    FTimelineAnimationRange DefaultRange;
    if (!TestTrue(TEXT("parse native default range"), ParseRange(Captured->GetObjectField(TEXT("default_range")), DefaultRange, CaptureError))) return false;
    TRange<FFrameNumber> Range;
    if (!TestTrue(TEXT("resolve native default range"), TrySectionRange(DefaultRange, TEXT("test"), Scene->GetTickResolution(), Range, CaptureError))) return false;
    TestTrue(TEXT("exported default is the native factory range"), Range == Track->CreateNewSection()->GetRange());
    TestFalse(TEXT("empty playback does not manufacture an empty section"), Range.IsEmpty());

    auto* Existing = Track->CreateNewSection();
    Track->AddSection(*Existing);
    if (!TryReadMovieSceneStructure(*Scene, TEXT("test"), {}, Structure, CaptureError)) return false;
    FTimelineAnimationStructure Current, Desired;
    if (!ParseStructure(Structure, Current, CaptureError)) return false;
    auto AuthoredRange = Structure->GetArrayField(TEXT("root_tracks"))[0]->AsObject()
        ->GetArrayField(TEXT("sections"))[0]->AsObject()->GetObjectField(TEXT("range"));
    AuthoredRange->GetObjectField(TEXT("upper"))->SetStringField(TEXT("kind"), TEXT("exclusive"));
    if (!ParseStructure(Structure, Desired, CaptureError)) return false;
    TestTrue(TEXT("explicit empty range follows native cleanup"), TryUpdateMovieSceneValues(*Scene, Current, Desired, CaptureError));
    TestEqual(TEXT("empty section is removed before expected-state capture"), Track->GetAllSections().Num(), 0);
    return !HasAnyErrors();
}

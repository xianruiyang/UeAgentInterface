#include "Misc/AutomationTest.h"

#include "ActorTreeItem.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Components/PointLightComponent.h"
#include "CoreGlobals.h"
#include "Editor.h"
#include "Engine/PointLight.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "Layout/WidgetPath.h"
#include "Framework/MultiBox/MultiBox.h"
#include "GenericPlatform/GenericWindow.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/PlatformTime.h"
#include "HAL/FileManager.h"
#include "ISequencer.h"
#include "ISequencerModule.h"
#include "ISceneOutlinerMode.h"
#include "ISceneOutlinerHierarchy.h"
#include "LevelSequence.h"
#include "LevelEditorSequencerIntegration.h"
#include "Misc/FileHelper.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "MovieScene.h"
#include "MovieScenePossessable.h"
#include "MovieSceneTracksSettings.h"
#include "PreviewScene.h"
#include "MVVM/ObjectBindingModelStorageExtension.h"
#include "MVVM/Selection/Selection.h"
#include "MVVM/ViewModels/ObjectBindingModel.h"
#include "MVVM/ViewModels/SequenceModel.h"
#include "MVVM/ViewModels/SequencerEditorViewModel.h"
#include "Sections/MovieScene3DTransformSection.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Sections/MovieSceneSubSection.h"
#include "Selection.h"
#include "SequencerSettings.h"
#include "SSceneOutliner.h"
#include "Tracks/MovieScene3DTransformTrack.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Tracks/MovieSceneSubTrack.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#include "Widgets/Input/SMenuAnchor.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STreeView.h"
#include "Vertical/T85/UeAgentT85SequenceIsolationPlaybackClient.h"

namespace UaiT85CandidateIsolation
{
using FValues = TMap<FString, FString>;

void DiagnoseIniDelta(FAutomationTestBase& Test, const FString& Before, const FString& Stage)
{
	FString After;
	FFileHelper::LoadFileToString(After, *GEditorPerProjectIni);
	const FString Root = FPaths::Combine(FPaths::ProjectDir(), TEXT("tmp/TROOT-UAI-208-graph-common-foundation/sequence-proposed-playback-override-isolation-r1/ini"));
	IFileManager::Get().MakeDirectory(*Root, true);
	FFileHelper::SaveStringToFile(Before, *FPaths::Combine(Root, Stage + TEXT("-before.ini")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	FFileHelper::SaveStringToFile(After, *FPaths::Combine(Root, Stage + TEXT("-after.ini")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	auto Parse = [](const FString& Text)
	{
		FConfigFile File;
		File.ProcessInputFileContents(Text, TEXT("ProposedIsolationDiagnostic"));
		TMap<FString, TArray<FString>> Values;
		for (const auto& Section : File)
			for (const auto& Entry : Section.Value) Values.FindOrAdd(Section.Key + TEXT("/") + Entry.Key.ToString()).Add(Entry.Value.GetValue());
		return Values;
	};
	const auto Old = Parse(Before);
	const auto New = Parse(After);
	TSet<FString> Keys;
	for (const auto& Pair : Old) Keys.Add(Pair.Key);
	for (const auto& Pair : New) Keys.Add(Pair.Key);
	int32 Changes = 0;
	for (const FString& Key : Keys)
	{
		const auto* Previous = Old.Find(Key);
		const auto* Current = New.Find(Key);
		if (Previous && Current && *Previous == *Current) continue;
		++Changes;
		const FString PreviousText = Previous ? FString::Join(*Previous, TEXT(" | ")) : TEXT("<absent>");
		const FString CurrentText = Current ? FString::Join(*Current, TEXT(" | ")) : TEXT("<absent>");
		Test.AddInfo(FString::Printf(TEXT("ProposedIniDelta stage=%s key=%s before=%s after=%s"), *Stage, *Key, *PreviousText.Left(240), *CurrentText.Left(240)));
	}
	Test.AddInfo(FString::Printf(TEXT("ProposedIniDelta stage=%s keys_changed=%d raw_equal=%d artifacts=%s"), *Stage, Changes, Before == After, *Root));
}

FValues PersistentValues(UObject* Object)
{
	FValues Values;
	for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
	{
		if (It->HasAnyPropertyFlags(CPF_Transient)) continue;
		FString Value;
		It->ExportText_InContainer(0, Value, Object, nullptr, Object, PPF_None);
		Values.Add(It->GetName(), MoveTemp(Value));
	}
	return Values;
}

TSharedPtr<SWidget> FindWidget(const TSharedRef<SWidget>& Root, FName Type)
{
	TArray<TSharedRef<SWidget>> Pending{Root};
	while (!Pending.IsEmpty())
	{
		const auto Widget = Pending.Pop(EAllowShrinking::No);
		if (Widget->GetType() == Type) return Widget;
		FChildren* Children = Widget->GetChildren();
		for (int32 Index = 0; Index < Children->Num(); ++Index) Pending.Add(Children->GetChildAt(Index));
	}
	return nullptr;
}

TArray<FString> Texts(const TSharedRef<SWidget>& Root)
{
	TArray<FString> Result;
	TArray<TSharedRef<SWidget>> Pending{Root};
	while (!Pending.IsEmpty())
	{
		const auto Widget = Pending.Pop(EAllowShrinking::No);
		if (Widget->GetType() == TEXT("STextBlock"))
		{
			const FString Text = StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString();
			if (!Text.IsEmpty()) Result.AddUnique(Text);
		}
		FChildren* Children = Widget->GetChildren();
		for (int32 Index = 0; Index < Children->Num(); ++Index) Pending.Add(Children->GetChildAt(Index));
	}
	return Result;
}

class SIsolationMenuHost : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SIsolationMenuHost) {} SLATE_END_ARGS()
	void Construct(const FArguments&) {}
	void SetContent(const TSharedRef<SWidget>& Widget) { ChildSlot[Widget]; }
	FPopupMethodReply OnQueryPopupMethod() const override
	{ return FPopupMethodReply::UseMethod(EPopupMethod::UseCurrentWindow); }
};

struct FFixture
{
	UWorld* World = nullptr;
	TArray<AActor*> OwnedActors;
	TStrongObjectPtr<ULevelSequence> Source;
	FGuid ChildGuid, ComponentGuid, ExistingGuid;
	~FFixture()
	{
		for (AActor* Actor : OwnedActors) if (IsValid(Actor)) World->DestroyActor(Actor);
	}
};

// Monitoring deliberately survives Close. There is no restoration path that can
// turn an intermediate source/world write into a passing isolation result.
struct FObservation
{
	FAutomationTestBase& Test;
	UWorld* World;
	FString Stage = TEXT("before-copy");
	TMap<UObject*, FValues> Objects;
	TMap<UPackage*, bool> Dirty;
	TSet<AActor*> Actors;
	TArray<TWeakObjectPtr<UObject>> SelectedActors, SelectedComponents, SelectedObjects;
	TArray<TSharedRef<SWindow>> Windows;
	TMap<TSharedRef<SWindow>, bool> WindowVisibility;
	TSharedPtr<SWidget> Focus;
	TArray<FString> Writes;
	TArray<TPair<USceneComponent*, FDelegateHandle>> TransformHandles;
	FDelegateHandle ModifiedHandle, PropertyHandle;
	FString IniBefore;
	bool bIniExisted = false;
	bool bOriginalWriteDetected = false;

	FObservation(FAutomationTestBase& InTest, FFixture& Fixture, UObject* Settings, UObject* TracksSettings)
		: Test(InTest), World(Fixture.World)
	{
		auto Add = [&](UObject* Object)
		{
			if (!Object || Objects.Contains(Object)) return;
			Objects.Add(Object, PersistentValues(Object));
			Dirty.Add(Object->GetOutermost(), Object->GetOutermost()->IsDirty());
			if (USceneComponent* Component = Cast<USceneComponent>(Object))
				TransformHandles.Emplace(Component, Component->TransformUpdated.AddLambda(
					[this](USceneComponent* Changed, EUpdateTransformFlags, ETeleportType)
					{ Writes.Add(Stage + TEXT(":TransformUpdated:") + Changed->GetPathName()); }));
		};
		Add(World); Add(Settings); Add(TracksSettings); Add(Fixture.Source.Get());
		Add(GEditor->GetSelectedActors()); Add(GEditor->GetSelectedComponents()); Add(GEditor->GetSelectedObjects());
		for (TObjectIterator<USequencerSettings> It; It; ++It) Add(*It);
		for (TObjectIterator<UMovieSceneTracksSettings> It; It; ++It) Add(*It);
		TArray<UObject*> Nested;
		GetObjectsWithOuter(Fixture.Source.Get(), Nested, true);
		for (UObject* Object : Nested) Add(Object);
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			Actors.Add(*It); Add(*It);
			TInlineComponentArray<UActorComponent*> Components(*It);
			for (UActorComponent* Component : Components) Add(Component);
		}
		GEditor->GetSelectedActors()->GetSelectedObjects(SelectedActors);
		GEditor->GetSelectedComponents()->GetSelectedObjects(SelectedComponents);
		GEditor->GetSelectedObjects()->GetSelectedObjects(SelectedObjects);
		Windows = FSlateApplication::Get().GetTopLevelWindows();
		TArray<TSharedRef<SWindow>> PendingWindows = Windows;
		while (!PendingWindows.IsEmpty())
		{
			const auto Window = PendingWindows.Pop(EAllowShrinking::No);
			const auto Native = Window->GetNativeWindow();
			WindowVisibility.Add(Window, Native && Native->IsVisible());
			PendingWindows.Append(Window->GetChildWindows());
		}
		Focus = FSlateApplication::Get().GetKeyboardFocusedWidget();
		bIniExisted = FFileHelper::LoadFileToString(IniBefore, *GEditorPerProjectIni);
		ModifiedHandle = FCoreUObjectDelegates::OnObjectModified.AddLambda([this](UObject* Object)
		{ if (Objects.Contains(Object)) Writes.Add(Stage + TEXT(":Modify:") + Object->GetPathName()); });
		PropertyHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddLambda([this](UObject* Object, FPropertyChangedEvent&)
		{ if (Objects.Contains(Object)) Writes.Add(Stage + TEXT(":PropertyChanged:") + Object->GetPathName()); });
	}
	~FObservation()
	{
		FCoreUObjectDelegates::OnObjectModified.Remove(ModifiedHandle);
		FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(PropertyHandle);
		for (const auto& Pair : TransformHandles) Pair.Key->TransformUpdated.Remove(Pair.Value);
	}
	void Check(const FString& Boundary, const TSharedPtr<SWindow>& OwnedWindow = nullptr)
	{
		for (const auto& Pair : Objects)
		{
			const FValues Current = PersistentValues(Pair.Key);
			if (!Pair.Value.OrderIndependentCompareEqual(Current))
			{
				bOriginalWriteDetected = true;
				FString ChangedProperty;
				for (const auto& Value : Pair.Value)
					if (!Current.Contains(Value.Key) || Current[Value.Key] != Value.Value) { ChangedProperty = Value.Key; break; }
				Test.AddError(Boundary + TEXT(": original persistent state changed: ") + Pair.Key->GetPathName() + TEXT(".") + ChangedProperty);
			}
		}
		for (const auto& Pair : Dirty) Test.TestEqual(Boundary + TEXT(": package dirty ") + Pair.Key->GetName(), Pair.Key->IsDirty(), Pair.Value);
		TSet<AActor*> CurrentActors;
		for (TActorIterator<AActor> It(World); It; ++It) CurrentActors.Add(*It);
		Test.TestTrue(Boundary + TEXT(": world actor identity set"), Actors.Difference(CurrentActors).IsEmpty() && CurrentActors.Difference(Actors).IsEmpty());
		TArray<TWeakObjectPtr<UObject>> Selection;
		GEditor->GetSelectedActors()->GetSelectedObjects(Selection);
		Test.TestTrue(Boundary + TEXT(": actor selection"), Selection == SelectedActors);
		GEditor->GetSelectedComponents()->GetSelectedObjects(Selection);
		Test.TestTrue(Boundary + TEXT(": component selection"), Selection == SelectedComponents);
		GEditor->GetSelectedObjects()->GetSelectedObjects(Selection);
		Test.TestTrue(Boundary + TEXT(": object selection"), Selection == SelectedObjects);
		auto CurrentWindows = FSlateApplication::Get().GetTopLevelWindows();
		if (OwnedWindow) CurrentWindows.Remove(OwnedWindow.ToSharedRef());
		Test.TestTrue(Boundary + TEXT(": original top level window identities"), CurrentWindows == Windows);
		for (const auto& Pair : WindowVisibility)
		{
			const auto Native = Pair.Key->GetNativeWindow();
			Test.TestEqual(Boundary + TEXT(": original window visibility"), bool(Native && Native->IsVisible()), Pair.Value);
		}
		Test.TestTrue(Boundary + TEXT(": keyboard focus"), Focus == FSlateApplication::Get().GetKeyboardFocusedWidget());
		FString IniNow;
		Test.TestEqual(Boundary + TEXT(": settings file existence"), FFileHelper::LoadFileToString(IniNow, *GEditorPerProjectIni), bIniExisted);
		Test.TestTrue(Boundary + TEXT(": settings file contents"), IniNow == IniBefore);
		bOriginalWriteDetected |= !Writes.IsEmpty();
		for (const FString& Write : Writes) Test.AddError(TEXT("Intermediate original write: ") + Write);
		Writes.Reset();
		Test.AddInfo(TEXT("CandidateIsolation boundary=") + Boundary);
		Stage = Boundary;
	}
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85SequenceCandidateIsolationTest,
	"UAI.Root.T85.LevelSequence.NativeTrackMenu.CandidateIsolation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85SequenceCandidateIsolationTest::RunTest(const FString&)
{
	using namespace UaiT85CandidateIsolation;
	using namespace UE::Sequencer;
	const auto Fixture = MakeShared<FFixture>();
	Fixture->World = GEditor->GetEditorWorldContext().World();
	if (!TestNotNull(TEXT("editor world"), Fixture->World)) return false;
	FActorSpawnParameters Spawn;
	Spawn.ObjectFlags = RF_Transient;
	for (int32 Index = 0; Index < 3; ++Index)
	{
		APointLight* Actor = Fixture->World->SpawnActor<APointLight>(FVector(Index * 123.0, 271.0, 419.0), FRotator::ZeroRotator, Spawn);
		if (!TestNotNull(TEXT("owned actor"), Actor)) return false;
		Fixture->OwnedActors.Add(Actor);
	}
	ULevelSequence* Source = NewObject<ULevelSequence>(GetTransientPackage(), NAME_None, RF_Transient | RF_Transactional);
	Fixture->Source.Reset(Source); Source->Initialize();
	UMovieScene* Scene = Source->GetMovieScene();
	Scene->SetPlaybackRange(0, 24000);
	Fixture->ChildGuid = Scene->AddPossessable(TEXT("IsolationChild"), APointLight::StaticClass());
	Fixture->ExistingGuid = Scene->AddPossessable(TEXT("IsolationExisting"), APointLight::StaticClass());
	UPointLightComponent* ChildComponent = CastChecked<UPointLightComponent>(CastChecked<APointLight>(Fixture->OwnedActors[0])->GetLightComponent());
	Fixture->ComponentGuid = Scene->AddPossessable(TEXT("IsolationComponent"), ChildComponent->GetClass());
	Scene->FindPossessable(Fixture->ComponentGuid)->SetParent(Fixture->ChildGuid, Scene);
	Source->BindPossessableObject(Fixture->ChildGuid, *Fixture->OwnedActors[0], Fixture->World);
	Source->BindPossessableObject(Fixture->ExistingGuid, *Fixture->OwnedActors[1], Fixture->World);
	Source->BindPossessableObject(Fixture->ComponentGuid, *ChildComponent, Fixture->OwnedActors[0]);
	UMovieScene3DTransformTrack* Track = Scene->AddTrack<UMovieScene3DTransformTrack>(Fixture->ChildGuid);
	UMovieScene3DTransformSection* Section = CastChecked<UMovieScene3DTransformSection>(Track->CreateNewSection());
	Section->SetRange(TRange<FFrameNumber>(0, 12000)); Track->AddSection(*Section);
	Section->GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>()[0]->SetDefault(9001.0);
	// The live transform differs from the animation even if a later Close restores it.
	TestNotEqual(TEXT("animation sentinel differs from live actor"), Fixture->OwnedActors[0]->GetActorLocation().X, 9001.0);
	FModuleManager::LoadModuleChecked<IModuleInterface>(TEXT("MovieSceneTools"));
	FModuleManager::LoadModuleChecked<ISequencerModule>(TEXT("Sequencer"));
	USequencerSettingsContainer::GetOrCreate<USequencerSettings>(TEXT("UaiT85CandidateIsolation"));
	USequencerSettingsContainer::GetOrCreate<UMovieSceneTracksSettings>(TEXT("UaiT85CandidateIsolation.TracksSettings"));
	const uint64 SetupFrame = GFrameCounter;
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Fixture, SetupFrame]()
	{
		if (GFrameCounter <= SetupFrame) return false;
		USequencerSettings* Settings = USequencerSettingsContainer::GetOrCreate<USequencerSettings>(TEXT("UaiT85CandidateIsolation"));
		UMovieSceneTracksSettings* TracksSettings = USequencerSettingsContainer::GetOrCreate<UMovieSceneTracksSettings>(TEXT("UaiT85CandidateIsolation.TracksSettings"));
		FObservation Observation(*this, *Fixture, Settings, TracksSettings);
		TArray<FString> Eligibility[2];
		for (int32 ReadOnlyIndex = 0; ReadOnlyIndex < 2; ++ReadOnlyIndex)
		{
			const FString Prefix = ReadOnlyIndex ? TEXT("readonly") : TEXT("editable");
			Observation.Stage = Prefix + TEXT(":duplicate");
			UPackage* Scratch = CreatePackage(*FString::Printf(TEXT("/Temp/UAI/Reflection/Proposed_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
			// Same duplication primitive and flags as Reflection DuplicateAsset.
			ULevelSequence* Proposed = CastChecked<ULevelSequence>(StaticDuplicateObject(Fixture->Source.Get(), Scratch, NAME_None, RF_Public | RF_Standalone | RF_Transactional));
			Proposed->ClearFlags(RF_Transient); Proposed->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
			ON_SCOPE_EXIT { Proposed->ClearFlags(RF_Public | RF_Standalone); };
			Observation.Check(Prefix + TEXT(":after-copy"));
			FSequencerInitParams Init;
			Init.RootSequence = Proposed; Init.PlaybackContext = Fixture->World;
			Init.ViewParams.UniqueName = TEXT("UaiT85CandidateIsolation");
			Init.ViewParams.bReadOnly = ReadOnlyIndex != 0;
			Observation.Stage = Prefix + TEXT(":initializing");
			const auto Sequencer = FModuleManager::LoadModuleChecked<ISequencerModule>(TEXT("Sequencer")).CreateSequencer(Init);
			Observation.Check(Prefix + TEXT(":after-init"));
			const auto Host = SNew(SIsolationMenuHost);
			const auto Window = SNew(SWindow).ClientSize(FVector2D(800, 800))[Host];
			FSlateApplication::Get().AddWindow(Window, false);
			auto Check = [&](const FString& Stage) { Observation.Check(Prefix + TEXT(":") + Stage, Window); };
			auto Open = [&](const TSharedRef<SMultiBoxWidget>& Box, const FString& Label) -> TSharedPtr<SMultiBoxWidget>
			{
				TArray<TSharedRef<SWidget>> Rows;
				for (const auto& Block : Box->GetMultiBox()->GetBlocks())
				{
					const auto Row = Block->MakeWidget(Box, EMultiBlockLocation::None, true, nullptr)->AsWidget();
					if (Texts(Row).Contains(Label)) Rows.Add(Row);
				}
				if (!TestEqual(TEXT("unique native submenu ") + Label, Rows.Num(), 1)) return nullptr;
				const auto Widget = FindWidget(Rows[0], TEXT("SMenuAnchor"));
				if (!TestTrue(TEXT("native submenu anchor"), Widget.IsValid())) return nullptr;
				const auto Anchor = StaticCastSharedPtr<SMenuAnchor>(Widget);
				Host->SetContent(Rows[0]); Window->SlatePrepass(); Anchor->SetIsOpen(true, false);
				TestTrue(TEXT("submenu remains on owned hidden host"), Anchor->GetMenuWindow() == Window);
				return StaticCastSharedPtr<SMultiBoxWidget>(FindWidget(Anchor.ToSharedRef(), TEXT("SMultiBoxWidget")));
			};
			auto Record = [&](const TSharedRef<SMultiBoxWidget>& Box, const FString& Path)
			{
				for (const auto& Block : Box->GetMultiBox()->GetBlocks())
				{
					const auto Row = Block->MakeWidget(Box, EMultiBlockLocation::None, true, nullptr)->AsWidget();
					const FUIAction& Action = Block->GetDirectActions();
					Eligibility[ReadOnlyIndex].Add(FString::Printf(TEXT("%s/%s|bound=%d|can=%d|enabled=%d|check=%d"),
						*Path, *FString::Join(Texts(Row), TEXT(";")), Action.IsBound(), Action.CanExecute(), Row->IsEnabled(), int32(Action.GetCheckState())));
				}
			};
			for (int32 BindingIndex = 0; BindingIndex < 2 && !Observation.bOriginalWriteDetected; ++BindingIndex)
			{
				const FGuid Binding = BindingIndex ? Fixture->ComponentGuid : Fixture->ChildGuid;
				UObject* Expected = BindingIndex ? Cast<UObject>(CastChecked<APointLight>(Fixture->OwnedActors[0])->GetLightComponent()) : Fixture->OwnedActors[0];
				const auto Bound = Sequencer->FindBoundObjects(Binding, MovieSceneSequenceID::Root);
				TestTrue(TEXT("duplicate resolves exact original world binding"), Bound.Num() == 1 && Bound[0].Get() == Expected);
				const auto Existing = Sequencer->FindBoundObjects(Fixture->ExistingGuid, MovieSceneSequenceID::Root);
				TestTrue(TEXT("duplicate resolves exact existing target"), Existing.Num() == 1 && Existing[0].Get() == Fixture->OwnedActors[1]);
				Check(TEXT("binding-resolution"));
				if (Observation.bOriginalWriteDetected) break;
				auto* Storage = Sequencer->GetViewModel()->GetRootSequenceModel()->CastDynamic<FObjectBindingModelStorageExtension>();
				const auto Model = Storage ? Storage->FindModelForObjectBinding(Binding) : nullptr;
				if (!TestTrue(TEXT("binding model"), Model.IsValid())) break;
				const auto Root = StaticCastSharedPtr<SMultiBoxWidget>(FindWidget(Model->GetAddTrackMenuContent(), TEXT("SMultiBoxWidget")));
				if (!TestTrue(TEXT("native root menu"), Root.IsValid())) break;
				Record(Root.ToSharedRef(), FString::FromInt(BindingIndex)); Check(TEXT("root-menu"));
				if (Observation.bOriginalWriteDetected) break;
				const auto Attach = Open(Root.ToSharedRef(), NSLOCTEXT("F3DAttachTrackEditor", "AddAttach", "Attach").ToString());
				if (!TestTrue(TEXT("native attach menu"), Attach.IsValid())) break;
				Record(Attach.ToSharedRef(), FString::FromInt(BindingIndex) + TEXT("/Attach"));
				int32 ExistingActions = 0;
				for (const auto& Block : Attach->GetMultiBox()->GetBlocks())
					if (Block->GetDirectActions().IsBound() && Texts(Block->MakeWidget(Attach.ToSharedRef(), EMultiBlockLocation::None, true, nullptr)->AsWidget()).Contains(TEXT("IsolationExisting"))) ++ExistingActions;
				TestEqual(TEXT("existing binding retains original callable action without execution"), ExistingActions, 1);
				Check(TEXT("existing-binding-menu"));
				if (Observation.bOriginalWriteDetected) break;
				const auto NewBinding = Open(Attach.ToSharedRef(), NSLOCTEXT("FActorPickerTrackEditor", "NewBinding", "New Binding").ToString());
				const auto Picker = NewBinding ? StaticCastSharedPtr<SSceneOutliner>(FindWidget(NewBinding.ToSharedRef(), TEXT("SSceneOutliner"))) : nullptr;
				if (!TestTrue(TEXT("original new actor picker"), Picker.IsValid())) break;
				Check(TEXT("new-binding-widget"));
				if (Observation.bOriginalWriteDetected) break;
				Picker->Tick(FGeometry(), FPlatformTime::Seconds(), 0.f);
				const auto Item = Picker->GetTreeItem(FSceneOutlinerTreeItemID(Fixture->OwnedActors[2]));
				const bool bEligible = Item && Item->CanInteract() && Item->CastTo<FActorTreeItem>() && Item->CastTo<FActorTreeItem>()->Actor.Get() == Fixture->OwnedActors[2];
				TestTrue(TEXT("unbound actor passes original native filter and interaction eligibility"), bEligible);
				Eligibility[ReadOnlyIndex].Add(FString::Printf(TEXT("%d/new-actor|eligible=%d"), BindingIndex, bEligible));
				Check(TEXT("new-binding-populated"));
			}
			Check(TEXT("before-close"));
			Observation.Stage = Prefix + TEXT(":closing"); Sequencer->Close();
			Check(TEXT("after-close"));
			FSlateApplication::Get().DestroyWindowImmediately(Window);
			Observation.Check(Prefix + TEXT(":after-host-destroy"));
			if (Observation.bOriginalWriteDetected) break;
		}
		TestTrue(TEXT("readonly preserves observed native action and actor eligibility"), Eligibility[0] == Eligibility[1]);
		if (Eligibility[0] != Eligibility[1])
		{
			for (const FString& Row : Eligibility[0]) if (!Eligibility[1].Contains(Row)) AddInfo(TEXT("Editable-only eligibility: ") + Row);
			for (const FString& Row : Eligibility[1]) if (!Eligibility[0].Contains(Row)) AddInfo(TEXT("Readonly-only eligibility: ") + Row);
		}
		AddInfo(FString::Printf(TEXT("CandidateIsolation readonly_compare_rows=%d no_action_execution=1 no_global_tick=1 no_force_evaluate=1"), Eligibility[0].Num()));
		return true;
	}));
	return true;
}

namespace UaiT85CandidateIsolation
{
// Explicit fixture preparation. None of this function belongs to the query.
TSharedPtr<FFixture> PrepareBorrowedFixture(FAutomationTestBase& Test)
{
	const auto Fixture = MakeShared<FFixture>();
	Fixture->World = GEditor->GetEditorWorldContext().World();
	if (!Test.TestNotNull(TEXT("borrowed fixture world"), Fixture->World)) return nullptr;
	FActorSpawnParameters Spawn;
	Spawn.ObjectFlags = RF_Transient;
	for (int32 Index = 0; Index < 3; ++Index)
	{
		APointLight* Actor = Fixture->World->SpawnActor<APointLight>(FVector(Index * 123.0, 271.0, 419.0), FRotator::ZeroRotator, Spawn);
		if (!Test.TestNotNull(TEXT("borrowed fixture actor"), Actor)) return nullptr;
		Fixture->OwnedActors.Add(Actor);
	}
	ULevelSequence* Source = NewObject<ULevelSequence>(GetTransientPackage(), NAME_None, RF_Transient | RF_Transactional);
	Fixture->Source.Reset(Source); Source->Initialize();
	UMovieScene* Scene = Source->GetMovieScene();
	Scene->SetPlaybackRange(0, 24000);
	Fixture->ChildGuid = Scene->AddPossessable(TEXT("BorrowedChild"), APointLight::StaticClass());
	Fixture->ExistingGuid = Scene->AddPossessable(TEXT("BorrowedExisting"), APointLight::StaticClass());
	UPointLightComponent* Component = CastChecked<UPointLightComponent>(CastChecked<APointLight>(Fixture->OwnedActors[0])->GetLightComponent());
	Fixture->ComponentGuid = Scene->AddPossessable(TEXT("BorrowedComponent"), Component->GetClass());
	Scene->FindPossessable(Fixture->ComponentGuid)->SetParent(Fixture->ChildGuid, Scene);
	Source->BindPossessableObject(Fixture->ChildGuid, *Fixture->OwnedActors[0], Fixture->World);
	Source->BindPossessableObject(Fixture->ExistingGuid, *Fixture->OwnedActors[1], Fixture->World);
	Source->BindPossessableObject(Fixture->ComponentGuid, *Component, Fixture->OwnedActors[0]);
	UMovieScene3DTransformTrack* Track = Scene->AddTrack<UMovieScene3DTransformTrack>(Fixture->ChildGuid);
	UMovieScene3DTransformSection* Section = CastChecked<UMovieScene3DTransformSection>(Track->CreateNewSection());
	Section->SetRange(TRange<FFrameNumber>(0, 12000)); Track->AddSection(*Section);
	Section->GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>()[0]->SetDefault(9001.0);
	return Fixture;
}

struct FBorrowedFixture
{
	TSharedPtr<FFixture> Objects;
	TSharedPtr<ISequencer> Sequencer;
	TStrongObjectPtr<ULevelSequence> Unopened;
	TStrongObjectPtr<ULevelSequence> Inner;
	UMovieSceneSubSection* SubSection = nullptr;
	TSharedPtr<SWindow> FocusSentinelWindow;
	TSharedPtr<SButton> FocusSentinel;
	int32 Phase = 0;
	uint64 PreparedFrame = 0;
	~FBorrowedFixture()
	{
		// Cleanup is outside the observed query and closes only the owned fixture.
		if (Sequencer)
		{
			FLevelEditorSequencerIntegration::Get().RemoveSequencer(Sequencer.ToSharedRef());
			Sequencer->Close();
		}
		if (FocusSentinelWindow) FSlateApplication::Get().DestroyWindowImmediately(FocusSentinelWindow.ToSharedRef());
	}
};

enum class EBorrowResult { Ready, OpenRequired, FocusMismatch, SelectionMismatch };

// Local test candidate for the proposed boundary, not a production registry.
// No context creation, focus switching, selection clearing, or Close is allowed.
EBorrowResult FindBorrowedContext(UMovieSceneSequence* Target, const FGuid& Binding, TSharedPtr<ISequencer>& Out)
{
	using namespace UE::Sequencer;
	Out.Reset();
	for (const TWeakPtr<ISequencer>& Weak : FLevelEditorSequencerIntegration::Get().GetSequencers())
	{
		const auto Candidate = Weak.Pin();
		if (!Candidate || Candidate->GetRootMovieSceneSequence() != Target) continue;
		if (Candidate->GetFocusedMovieSceneSequence() != Target) return EBorrowResult::FocusMismatch;
		auto* Storage = Candidate->GetViewModel()->GetRootSequenceModel()->CastDynamic<FObjectBindingModelStorageExtension>();
		const auto Model = Storage ? Storage->FindModelForObjectBinding(Binding) : nullptr;
		if (!Model) return EBorrowResult::FocusMismatch;
		const auto Selection = Candidate->GetViewModel()->GetSelection();
		if (Selection->Outliner.IsSelected(Model))
			for (TViewModelPtr<FObjectBindingModel> Selected : Selection->Outliner.Filter<FObjectBindingModel>())
				if (Selected->GetObjectGuid() != Binding) return EBorrowResult::SelectionMismatch;
		Out = Candidate;
		return EBorrowResult::Ready;
	}
	return EBorrowResult::OpenRequired;
}

TArray<FGuid> SelectedBindings(ISequencer& Sequencer)
{
	using namespace UE::Sequencer;
	TArray<FGuid> Result;
	for (TViewModelPtr<FObjectBindingModel> Selected : Sequencer.GetViewModel()->GetSelection()->Outliner.Filter<FObjectBindingModel>())
		Result.Add(Selected->GetObjectGuid());
	Result.Sort();
	return Result;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85SequenceBorrowedCandidateIsolationTest,
	"UAI.Root.T85.LevelSequence.NativeTrackMenu.BorrowedCandidateIsolation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85SequenceBorrowedCandidateIsolationTest::RunTest(const FString& Parameters)
{
	using namespace UaiT85CandidateIsolation;
	using namespace UE::Sequencer;
	const bool bNoTickCandidates = Parameters == TEXT("NoTickCandidates");
	const bool bDetachPicker = Parameters == TEXT("DetachPicker") || bNoTickCandidates;
	const auto State = MakeShared<FBorrowedFixture>();
	State->Objects = PrepareBorrowedFixture(*this);
	if (!State->Objects) return false;
	State->Unopened.Reset(NewObject<ULevelSequence>(GetTransientPackage(), NAME_None, RF_Transient));
	State->Unopened->Initialize();
	State->Inner.Reset(NewObject<ULevelSequence>(GetTransientPackage(), NAME_None, RF_Transient));
	State->Inner->Initialize(); State->Inner->GetMovieScene()->SetPlaybackRange(0, 24000);
	UMovieSceneSubTrack* SubTrack = State->Objects->Source->GetMovieScene()->AddTrack<UMovieSceneSubTrack>();
	State->SubSection = SubTrack->AddSequence(State->Inner.Get(), 0, 24000);
	FModuleManager::LoadModuleChecked<IModuleInterface>(TEXT("MovieSceneTools"));
	FSequencerInitParams Init;
	Init.RootSequence = State->Objects->Source.Get(); Init.PlaybackContext = State->Objects->World;
	Init.ViewParams.UniqueName = TEXT("UaiT85BorrowedCandidateIsolation");
	State->Sequencer = FModuleManager::LoadModuleChecked<ISequencerModule>(TEXT("Sequencer")).CreateSequencer(Init);
	FLevelEditorSequencerIntegrationOptions Options;
	FLevelEditorSequencerIntegration::Get().AddSequencer(State->Sequencer.ToSharedRef(), Options);
	if (bDetachPicker)
	{
		// Owned worker fixture preparation: a non-null focus sentinel prevents
		// null-to-null focus writes from masquerading as isolation success.
		State->FocusSentinel = SNew(SButton).Text(FText::FromString(TEXT("Isolation focus sentinel")));
		State->FocusSentinelWindow = SNew(SWindow)[State->FocusSentinel.ToSharedRef()];
		FSlateApplication::Get().AddWindow(State->FocusSentinelWindow.ToSharedRef(), false);
		FSlateApplication::Get().SetKeyboardFocus(State->FocusSentinel, EFocusCause::SetDirectly);
	}
	// Prepare a real focused subsequence as the first negative context.
	State->Sequencer->FocusSequenceInstance(*State->SubSection);
	State->Objects->OwnedActors[0]->SetActorLocation(FVector(0.0, 271.0, 419.0));
	State->PreparedFrame = GFrameCounter;
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, State, bDetachPicker, bNoTickCandidates]()
	{
		if (GFrameCounter <= State->PreparedFrame) return false;
		auto& Fixture = *State->Objects;
		// The registered editor may evaluate naturally during the preparation
		// frame. Establish the owned fixture sentinel only after that frame,
		// before installing query observers; never restore it during a query.
		Fixture.OwnedActors[0]->SetActorLocation(FVector(0.0, 271.0, 419.0));
		bool bStop = false;
		TSharedPtr<SSceneOutliner> ComparisonPicker;
		TMap<AActor*, bool> QueryCandidates;
		{
			USequencerSettings* Settings = State->Sequencer->GetSequencerSettings();
			UMovieSceneTracksSettings* TracksSettings = USequencerSettingsContainer::GetOrCreate<UMovieSceneTracksSettings>(TEXT("UaiT85BorrowedCandidateIsolation.TracksSettings"));
			FObservation Observation(*this, Fixture, Settings, TracksSettings);
			if (bDetachPicker) TestTrue(TEXT("query begins with original non-null focus sentinel"), FSlateApplication::Get().GetKeyboardFocusedWidget() == State->FocusSentinel);
			const TArray<FGuid> SelectionBefore = SelectedBindings(*State->Sequencer);
			UMovieSceneSequence* FocusBefore = State->Sequencer->GetFocusedMovieSceneSequence();
			auto Check = [&](const FString& Stage)
			{
				Observation.Check(TEXT("borrowed:") + Stage);
				TestTrue(Stage + TEXT(": internal outliner selection unchanged"), SelectedBindings(*State->Sequencer) == SelectionBefore);
				TestTrue(Stage + TEXT(": focused sequence unchanged"), State->Sequencer->GetFocusedMovieSceneSequence() == FocusBefore);
			};
			TestNotEqual(TEXT("borrowed query has live animation sentinel"), Fixture.OwnedActors[0]->GetActorLocation().X, 9001.0);
			TSharedPtr<ISequencer> Borrowed;
			Check(TEXT("query-start"));
			if (State->Phase == 0)
			{
				TestTrue(TEXT("unopened target requires explicit editor open"), FindBorrowedContext(State->Unopened.Get(), Fixture.ChildGuid, Borrowed) == EBorrowResult::OpenRequired && !Borrowed);
				Check(TEXT("unopened-rejected"));
				TestTrue(TEXT("actual focused child differs from requested root"), FocusBefore == State->Inner.Get());
				TestTrue(TEXT("wrong focus rejected without switching"), FindBorrowedContext(Fixture.Source.Get(), Fixture.ChildGuid, Borrowed) == EBorrowResult::FocusMismatch && !Borrowed);
				Check(TEXT("focus-rejected"));
			}
			else if (State->Phase == 1)
			{
				TestEqual(TEXT("fixture retains both real selected bindings"), SelectionBefore.Num(), 2);
				TestTrue(TEXT("native multi binding context rejected without clearing selection"), FindBorrowedContext(Fixture.Source.Get(), Fixture.ChildGuid, Borrowed) == EBorrowResult::SelectionMismatch && !Borrowed);
				Check(TEXT("multiselection-rejected"));
			}
			else
			{
				// This unselected component is a genuinely single-binding native
				// context even while the two other bindings stay selected.
				TestEqual(TEXT("positive query preserves the two other selected bindings"), SelectionBefore.Num(), 2);
				TestTrue(TEXT("exact already initialized context borrowed"), FindBorrowedContext(Fixture.Source.Get(), Fixture.ComponentGuid, Borrowed) == EBorrowResult::Ready && Borrowed == State->Sequencer);
				if (Borrowed)
				{
					const auto Bound = Borrowed->FindBoundObjects(Fixture.ComponentGuid, MovieSceneSequenceID::Root);
					TestTrue(TEXT("borrowed source binding resolves exact world component"), Bound.Num() == 1 && Bound[0].Get() == CastChecked<APointLight>(Fixture.OwnedActors[0])->GetLightComponent());
					Check(TEXT("binding-resolved"));
					auto* Storage = Borrowed->GetViewModel()->GetRootSequenceModel()->CastDynamic<FObjectBindingModelStorageExtension>();
					const auto Model = Storage ? Storage->FindModelForObjectBinding(Fixture.ComponentGuid) : nullptr;
					const auto Root = Model ? StaticCastSharedPtr<SMultiBoxWidget>(FindWidget(Model->GetAddTrackMenuContent(), TEXT("SMultiBoxWidget"))) : nullptr;
					TestTrue(TEXT("borrowed original root menu"), Root.IsValid());
					Check(TEXT("root-menu"));
					if (Root && !Observation.bOriginalWriteDetected)
					{
						// A hidden owned host is only a menu rendering surface; the
						// observation explicitly permits its own identity, not focus changes.
						const auto Host = SNew(SIsolationMenuHost);
						const auto Window = SNew(SWindow).ClientSize(FVector2D(800, 800))[Host];
						FSlateApplication::Get().AddWindow(Window, false);
						auto MenuCheck = [&](const FString& Stage)
						{
							Observation.Check(TEXT("borrowed:") + Stage, Window);
							TestTrue(Stage + TEXT(": internal selection unchanged"), SelectedBindings(*Borrowed) == SelectionBefore);
						};
						TArray<TSharedPtr<SMenuAnchor>> OpenedAnchors;
						auto Open = [&](const TSharedRef<SMultiBoxWidget>& Box, const FString& Label) -> TSharedPtr<SMultiBoxWidget>
						{
							for (const auto& Block : Box->GetMultiBox()->GetBlocks())
							{
								const auto Row = Block->MakeWidget(Box, EMultiBlockLocation::None, true, nullptr)->AsWidget();
								if (!Texts(Row).Contains(Label)) continue;
								const auto Widget = FindWidget(Row, TEXT("SMenuAnchor"));
								if (!Widget) return nullptr;
								Host->SetContent(Row); Window->SlatePrepass();
								const auto Anchor = StaticCastSharedPtr<SMenuAnchor>(Widget);
								Anchor->SetIsOpen(true, false);
								OpenedAnchors.Add(Anchor);
								return StaticCastSharedPtr<SMultiBoxWidget>(FindWidget(Anchor.ToSharedRef(), TEXT("SMultiBoxWidget")));
							}
							return nullptr;
						};
						const auto Attach = Open(Root.ToSharedRef(), NSLOCTEXT("F3DAttachTrackEditor", "AddAttach", "Attach").ToString());
						TestTrue(TEXT("borrowed original attach menu"), Attach.IsValid());
						MenuCheck(TEXT("attach-menu"));
						if (Attach && !Observation.bOriginalWriteDetected)
						{
							int32 ExistingActions = 0;
							for (const auto& Block : Attach->GetMultiBox()->GetBlocks())
								if (Block->GetDirectActions().IsBound() && Texts(Block->MakeWidget(Attach.ToSharedRef(), EMultiBlockLocation::None, true, nullptr)->AsWidget()).Contains(TEXT("BorrowedExisting")))
								{
									TestTrue(TEXT("existing binding native action eligible without execution"), Block->GetDirectActions().CanExecute());
									++ExistingActions;
								}
							TestEqual(TEXT("existing binding candidate unique"), ExistingActions, 1);
							MenuCheck(TEXT("existing-binding"));
							const auto New = Open(Attach.ToSharedRef(), NSLOCTEXT("FActorPickerTrackEditor", "NewBinding", "New Binding").ToString());
							const auto Picker = New ? StaticCastSharedPtr<SSceneOutliner>(FindWidget(New.ToSharedRef(), TEXT("SSceneOutliner"))) : nullptr;
							TestTrue(TEXT("borrowed original actor picker"), Picker.IsValid());
							MenuCheck(TEXT("actor-picker-created"));
							if (Picker && !Observation.bOriginalWriteDetected)
							{
								if (bDetachPicker)
								{
									for (int32 Index = OpenedAnchors.Num() - 1; Index >= 0; --Index) OpenedAnchors[Index]->SetIsOpen(false, false);
									Host->SetContent(SNullWidget::NullWidget);
									FWidgetPath DetachedPath;
									FSlateApplication::Get().GeneratePathToWidgetUnchecked(Picker.ToSharedRef(), DetachedPath);
									TestFalse(TEXT("original picker has no valid Slate widget path"), DetachedPath.IsValid());
									MenuCheck(TEXT("original-picker-detached"));
								}
								if (bNoTickCandidates)
								{
									// GetMode exposes a const view of the mode owned by this
									// newly constructed mutable picker. Call its public lifecycle
									// and factory; do not replace its callback or inspect members.
									ISceneOutlinerMode* Mode = const_cast<ISceneOutlinerMode*>(Picker->GetMode());
									Mode->Rebuild();
									TArray<FSceneOutlinerTreeItemPtr> NativeItems;
									Mode->GetHierarchy()->CreateItems(NativeItems);
									for (const auto& NativeItem : NativeItems)
										if (const FActorTreeItem* ActorItem = NativeItem->CastTo<FActorTreeItem>())
											QueryCandidates.Add(ActorItem->Actor.Get(), NativeItem->CanInteract());
									TestTrue(TEXT("original hierarchy supplies eligible unbound actor"), QueryCandidates.FindRef(Fixture.OwnedActors[2]));
									const auto Positive = Mode->CreateItemFor<FActorTreeItem>(Fixture.OwnedActors[2]);
									TestTrue(TEXT("original mode factory agrees on eligible actor"), Positive && Positive->CanInteract());
									AActor* WorldSettings = Fixture.World->GetWorldSettings();
									TestFalse(TEXT("original hierarchy excludes ineligible world settings"), QueryCandidates.Contains(WorldSettings));
									TestFalse(TEXT("original mode factory rejects world settings"), Mode->CreateItemFor<FActorTreeItem>(WorldSettings).IsValid());
									ComparisonPicker = Picker;
									MenuCheck(TEXT("no-tick-native-hierarchy-candidates"));
								}
								else
								{
									Picker->Tick(FGeometry(), FPlatformTime::Seconds(), 0.f);
									const auto Item = Picker->GetTreeItem(FSceneOutlinerTreeItemID(Fixture.OwnedActors[2]));
									TestTrue(TEXT("unbound actor present and native-interactable"), Item && Item->CanInteract() && Item->CastTo<FActorTreeItem>() && Item->CastTo<FActorTreeItem>()->Actor.Get() == Fixture.OwnedActors[2]);
									MenuCheck(TEXT("actor-picker-populated"));
								}
							}
						}
						FSlateApplication::Get().DestroyWindowImmediately(Window);
					}
				}
				Check(TEXT("query-end"));
			}
			bStop = Observation.bOriginalWriteDetected || HasAnyErrors();
		}
		if (!bStop && ComparisonPicker)
		{
			// Independent oracle, AFTER query-end and observer destruction. Its
			// known focus side effect is not part of, or restoration of, the query.
			AddInfo(TEXT("NoTickCandidates query observation ended; begin native Tick tree oracle"));
			ComparisonPicker->Tick(FGeometry(), FPlatformTime::Seconds(), 0.f);
			TArray<FSceneOutlinerTreeItemPtr> Pending;
			for (const auto& Root : ComparisonPicker->GetTree().GetRootItems()) Pending.Add(Root);
			TMap<AActor*, bool> TreeCandidates;
			TSet<FSceneOutlinerTreeItemID> Visited;
			while (!Pending.IsEmpty())
			{
				const auto Item = Pending.Pop(EAllowShrinking::No);
				if (!Item || Visited.Contains(Item->GetID())) continue;
				Visited.Add(Item->GetID());
				if (!Item->Flags.bIsFilteredOut)
					if (const FActorTreeItem* ActorItem = Item->CastTo<FActorTreeItem>()) TreeCandidates.Add(ActorItem->Actor.Get(), Item->CanInteract());
				for (const auto& Child : Item->GetChildren()) if (const auto Pinned = Child.Pin()) Pending.Add(Pinned);
			}
			TestTrue(TEXT("no-Tick candidates equal full original actor tree identities and interaction eligibility"), QueryCandidates.OrderIndependentCompareEqual(TreeCandidates));
			AddInfo(FString::Printf(TEXT("NoTickCandidates hierarchy_actors=%d native_tree_actors=%d query_no_tick=1"), QueryCandidates.Num(), TreeCandidates.Num()));
		}
		if (bStop || State->Phase == 2) return true;
		// Explicit preparation of the next independent query context. This does
		// not repair a failed query; any earlier failed oracle exits above.
		if (State->Phase == 0)
		{
			State->Sequencer->PopToSequenceInstance(MovieSceneSequenceID::Root);
			auto* Storage = State->Sequencer->GetViewModel()->GetRootSequenceModel()->CastDynamic<FObjectBindingModelStorageExtension>();
			State->Sequencer->GetViewModel()->GetSelection()->Outliner.Select(Storage->FindModelForObjectBinding(Fixture.ChildGuid));
			State->Sequencer->GetViewModel()->GetSelection()->Outliner.Select(Storage->FindModelForObjectBinding(Fixture.ExistingGuid));
		}
		Fixture.OwnedActors[0]->SetActorLocation(FVector(0.0, 271.0, 419.0));
		++State->Phase;
		State->PreparedFrame = GFrameCounter;
		return false;
	}));
	return true;
}

class FUeAgentT85BorrowedIsolationVariantBase : public FUeAgentT85SequenceBorrowedCandidateIsolationTest
{
public:
	FUeAgentT85BorrowedIsolationVariantBase(const FString& Name, bool)
		: FUeAgentT85SequenceBorrowedCandidateIsolationTest(Name) {}
};

IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FUeAgentT85SequenceDetachedPickerIsolationTest,
	FUeAgentT85BorrowedIsolationVariantBase,
	"UAI.Root.T85.LevelSequence.NativeTrackMenu.DetachedPickerIsolation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85SequenceDetachedPickerIsolationTest::RunTest(const FString&)
{
	return FUeAgentT85SequenceBorrowedCandidateIsolationTest::RunTest(TEXT("DetachPicker"));
}

IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FUeAgentT85SequenceNoTickCandidateIsolationTest,
	FUeAgentT85BorrowedIsolationVariantBase,
	"UAI.Root.T85.LevelSequence.NativeTrackMenu.NoTickCandidateIsolation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85SequenceNoTickCandidateIsolationTest::RunTest(const FString&)
{
	return FUeAgentT85SequenceBorrowedCandidateIsolationTest::RunTest(TEXT("NoTickCandidates"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85SequenceProposedNativeActionIsolationTest,
	"UAI.Root.T85.LevelSequence.NativeTrackMenu.ProposedNativeActionIsolation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85SequenceProposedNativeActionIsolationTest::RunTest(const FString& Parameters)
{
	using namespace UaiT85CandidateIsolation;
	using namespace UE::Sequencer;
	const bool bOverridePlayback = Parameters == TEXT("OverridePlayback");
	const auto Fixture = PrepareBorrowedFixture(*this);
	if (!Fixture) return false;
	FGuid DeniedBinding;
	if (bOverridePlayback)
	{
		DeniedBinding = Fixture->Source->GetMovieScene()->AddPossessable(TEXT("UnmappedMustNotFallBack"), Fixture->OwnedActors[2]->GetClass());
		Fixture->Source->BindPossessableObject(DeniedBinding, *Fixture->OwnedActors[2], Fixture->World);
	}
	FModuleManager::LoadModuleChecked<IModuleInterface>(TEXT("MovieSceneTools"));
	FSequencerInitParams Init;
	Init.RootSequence = Fixture->Source.Get(); Init.PlaybackContext = Fixture->World;
	Init.ViewParams.UniqueName = TEXT("UaiT85ProposedNativeAction");
	const auto SourceContext = FModuleManager::LoadModuleChecked<ISequencerModule>(TEXT("Sequencer")).CreateSequencer(Init);
	// Explicit preparation of the source editor, before the observed Proposed
	// phase. Never reset this borrowed context or execute its menu action.
	SourceContext->SetInfiniteKeyAreas(true);
	SourceContext->GetSequencerSettings()->SetAutoSetTrackDefaults(true);
	const auto FocusSentinel = SNew(SButton).Text(FText::FromString(TEXT("Proposed action focus sentinel")));
	const auto FocusWindow = SNew(SWindow)[FocusSentinel];
	FSlateApplication::Get().AddWindow(FocusWindow, false);
	FSlateApplication::Get().SetKeyboardFocus(FocusSentinel, EFocusCause::SetDirectly);
	const uint64 PreparedFrame = GFrameCounter;
	FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FFunctionLatentCommand>([this, Fixture, SourceContext, Init, PreparedFrame, FocusWindow, FocusSentinel, bOverridePlayback, DeniedBinding]()
	{
		if (GFrameCounter <= PreparedFrame) return false;
		FString BeforeCloseIni;
		ON_SCOPE_EXIT { if (bOverridePlayback && !BeforeCloseIni.IsEmpty()) DiagnoseIniDelta(*this, BeforeCloseIni, TEXT("after-fixture-cleanup")); };
		ON_SCOPE_EXIT { SourceContext->Close(); };
		ON_SCOPE_EXIT { FSlateApplication::Get().DestroyWindowImmediately(FocusWindow); };
		UPointLightComponent* Component = CastChecked<UPointLightComponent>(CastChecked<APointLight>(Fixture->OwnedActors[0])->GetLightComponent());
		constexpr float ExpectedIntensity = 1729.25f;
		Component->SetIntensity(ExpectedIntensity);
		Fixture->OwnedActors[0]->SetActorLocation(FVector(0.0, 271.0, 419.0));
		USequencerSettings* Settings = SourceContext->GetSequencerSettings();
		UMovieSceneTracksSettings* TracksSettings = USequencerSettingsContainer::GetOrCreate<UMovieSceneTracksSettings>(TEXT("UaiT85ProposedNativeAction.TracksSettings"));
		FObservation Observation(*this, *Fixture, Settings, TracksSettings);
		TestTrue(TEXT("Proposed phase begins with non-null fixture focus"), FSlateApplication::Get().GetKeyboardFocusedWidget() == FocusSentinel);
		const auto SourceSelection = SelectedBindings(*SourceContext);
		auto Check = [&](const FString& Stage)
		{
			Observation.Check(TEXT("proposed-action:") + Stage);
			TestTrue(Stage + TEXT(": original context root/focus retained"), SourceContext->GetRootMovieSceneSequence() == Fixture->Source.Get()
				&& SourceContext->GetFocusedMovieSceneSequence() == Fixture->Source.Get());
			TestTrue(Stage + TEXT(": original context selection retained"), SourceSelection == SelectedBindings(*SourceContext));
		};
		Check(TEXT("before-copy"));
		TUniquePtr<FPreviewScene> IsolatedScene;
		TStrongObjectPtr<UUeAgentT85SequenceIsolationPlaybackClient> PlaybackClient;
		UPointLightComponent* ExpectedBoundComponent = Component;
		AActor* IsolatedChild = nullptr;
		if (bOverridePlayback)
		{
			IsolatedScene = MakeUnique<FPreviewScene>();
			PlaybackClient.Reset(NewObject<UUeAgentT85SequenceIsolationPlaybackClient>(GetTransientPackage()));
			TArray<AActor*> Clones;
			for (AActor* Original : Fixture->OwnedActors)
			{
				FActorSpawnParameters Spawn;
				Spawn.Template = Original;
				Spawn.ObjectFlags = RF_Transient;
				Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
				// PostSpawnInitialize otherwise multiplies the copied native root
				// transform by this transform, applying the original pose twice.
				Spawn.TransformScaleMethod = ESpawnActorScaleMethod::OverrideRootScale;
				const FTransform OriginalTransform = Original->GetActorTransform();
				AActor* Clone = IsolatedScene->GetWorld()->SpawnActor(Original->GetClass(), &OriginalTransform, Spawn);
				if (!TestNotNull(TEXT("native template Actor copy in isolated world"), Clone)) return true;
				TestTrue(TEXT("copied Actor retains type and original transform"), Clone != Original && Clone->GetClass() == Original->GetClass()
					&& Clone->GetActorTransform().Equals(OriginalTransform) && Clone->GetWorld() != Fixture->World);
				Clones.Add(Clone);
			}
			IsolatedChild = Clones[0];
			ExpectedBoundComponent = CastChecked<UPointLightComponent>(CastChecked<APointLight>(IsolatedChild)->GetLightComponent());
			TestTrue(TEXT("native component copy retains identity shape and original property value"), ExpectedBoundComponent != Component
				&& ExpectedBoundComponent->GetClass() == Component->GetClass() && ExpectedBoundComponent->GetFName() == Component->GetFName()
				&& ExpectedBoundComponent->GetOwner() == IsolatedChild && ExpectedBoundComponent->Intensity == ExpectedIntensity);
			PlaybackClient->RootBindings.Add(Fixture->ChildGuid, IsolatedChild);
			PlaybackClient->RootBindings.Add(Fixture->ExistingGuid, Clones[1]);
			PlaybackClient->RootBindings.Add(Fixture->ComponentGuid, ExpectedBoundComponent);
			Check(TEXT("after-native-bound-object-copies"));
		}
		UPackage* Scratch = CreatePackage(*FString::Printf(TEXT("/Temp/UAI/Reflection/Proposed_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
		ULevelSequence* Proposed = CastChecked<ULevelSequence>(StaticDuplicateObject(Fixture->Source.Get(), Scratch, NAME_None, RF_Public | RF_Standalone | RF_Transactional));
		Proposed->ClearFlags(RF_Transient); Proposed->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
		ON_SCOPE_EXIT { Proposed->ClearFlags(RF_Public | RF_Standalone); };
		Check(TEXT("after-reflection-copy"));
		FSequencerInitParams ProposedInit = Init;
		ProposedInit.RootSequence = Proposed;
		if (PlaybackClient.IsValid())
		{
			ProposedInit.PlaybackContext = IsolatedScene->GetWorld();
			ProposedInit.PlaybackClient = PlaybackClient.Get();
		}
		Observation.Stage = TEXT("proposed-action:initializing-proposed-context");
		const auto ProposedContext = FModuleManager::LoadModuleChecked<ISequencerModule>(TEXT("Sequencer")).CreateSequencer(ProposedInit);
		Check(TEXT("after-proposed-context-init"));
		if (!Observation.bOriginalWriteDetected && !HasAnyErrors())
		{
			const auto Bound = ProposedContext->FindBoundObjects(Fixture->ComponentGuid, MovieSceneSequenceID::Root);
			TestTrue(TEXT("Proposed copied binding resolves exact expected component"), Bound.Num() == 1 && Bound[0].Get() == ExpectedBoundComponent);
			if (PlaybackClient.IsValid())
			{
				for (const auto& Override : PlaybackClient->RootBindings)
				{
					const auto Objects = ProposedContext->FindBoundObjects(Override.Key, MovieSceneSequenceID::Root);
					TestTrue(TEXT("every evaluated binding resolves only its native copy"), Objects.Num() == 1 && Objects[0].Get() == Override.Value.Get());
				}
				TestTrue(TEXT("unmapped real binding cannot fall back to original world"), ProposedContext->FindBoundObjects(DeniedBinding, MovieSceneSequenceID::Root).IsEmpty());
				TestTrue(TEXT("reverse native object handle retains original binding GUID"), ProposedContext->GetHandleToObject(ExpectedBoundComponent, false) == Fixture->ComponentGuid);
				Check(TEXT("override-and-default-denial-readback"));
			}
			UMovieScene* Scene = Proposed->GetMovieScene();
			const int32 BindingsBefore = Scene->GetBindings().Num();
			const auto* TargetBefore = Scene->FindBinding(Fixture->ComponentGuid);
			TestTrue(TEXT("Proposed target starts without tracks"), TargetBefore && TargetBefore->GetTracks().IsEmpty());
			const auto* NonTarget = Scene->FindBinding(Fixture->ChildGuid);
			if (TestTrue(TEXT("Proposed is nonempty with copied transform track"), NonTarget && NonTarget->GetTracks().Num() == 1))
			{
				UMovieSceneTrack* ExistingTrack = NonTarget->GetTracks()[0];
				const FValues ExistingTrackBefore = PersistentValues(ExistingTrack);
				UMovieSceneSection* ExistingSection = ExistingTrack->GetAllSections()[0];
				const FValues ExistingSectionBefore = PersistentValues(ExistingSection);
				auto* Storage = ProposedContext->GetViewModel()->GetRootSequenceModel()->CastDynamic<FObjectBindingModelStorageExtension>();
				const auto Model = Storage ? Storage->FindModelForObjectBinding(Fixture->ComponentGuid) : nullptr;
				const auto Menu = Model ? StaticCastSharedPtr<SMultiBoxWidget>(FindWidget(Model->GetAddTrackMenuContent(), TEXT("SMultiBoxWidget"))) : nullptr;
				const FFloatProperty* Property = FindFProperty<FFloatProperty>(Component->GetClass(), TEXT("Intensity"));
				TArray<TSharedRef<const FMultiBlock>> Matches;
				if (Menu && Property)
					for (const auto& Block : Menu->GetMultiBox()->GetBlocks())
						if (Block->GetDirectActions().IsBound() && Texts(Block->MakeWidget(Menu.ToSharedRef(), EMultiBlockLocation::None, true, nullptr)->AsWidget()).Contains(Property->GetDisplayNameText().ToString())) Matches.Add(Block);
				Check(TEXT("proposed-native-menu-resolved"));
				if (TestEqual(TEXT("exact native Float leaf rebuilt on Proposed"), Matches.Num(), 1) && !Observation.bOriginalWriteDetected && !HasAnyErrors())
				{
					Observation.Stage = TEXT("proposed-action:executing-native-float-leaf");
					TestTrue(TEXT("Proposed native leaf executes its original action"), Matches[0]->GetDirectActions().Execute());
					Check(TEXT("after-native-action"));
					if (IsolatedChild)
					{
						TestEqual(TEXT("non-target animation still evaluates on the copied Actor"), IsolatedChild->GetActorLocation().X, 9001.0);
						TestEqual(TEXT("original Actor keeps independent live sentinel"), Fixture->OwnedActors[0]->GetActorLocation().X, 0.0);
						TestTrue(TEXT("original playback override callback was used"), PlaybackClient->OverrideCalls > 0 && PlaybackClient->DeniedCalls > 0);
					}
					const auto* TargetAfter = Scene->FindBinding(Fixture->ComponentGuid);
					TestEqual(TEXT("native action creates no additional binding"), Scene->GetBindings().Num(), BindingsBefore);
					if (TestTrue(TEXT("only Proposed target gains one track"), TargetAfter && TargetAfter->GetTracks().Num() == 1))
					{
						const auto* Track = Cast<UMovieSceneFloatTrack>(TargetAfter->GetTracks()[0]);
						TestTrue(TEXT("native track has independently expected property and one section"), Track && Track->GetPropertyName() == TEXT("Intensity")
							&& Track->GetPropertyPath().ToString() == TEXT("Intensity") && Track->GetAllSections().Num() == 1);
						const auto* Section = Track && Track->GetAllSections().Num() == 1 ? Cast<UMovieSceneFloatSection>(Track->GetAllSections()[0]) : nullptr;
						const auto* Channel = Section ? Section->GetChannelProxy().GetChannel<FMovieSceneFloatChannel>(0) : nullptr;
						float Value = 0.f;
						TestTrue(TEXT("native section unbounded with exact fixture intensity"), Section && Section->GetRange().GetLowerBound().IsOpen() && Section->GetRange().GetUpperBound().IsOpen()
							&& Channel && Channel->Evaluate(FFrameTime(0), Value) && Value == ExpectedIntensity);
					}
					TestTrue(TEXT("Proposed non-target track retained"), ExistingTrackBefore.OrderIndependentCompareEqual(PersistentValues(ExistingTrack)));
					TestTrue(TEXT("Proposed non-target section retained"), ExistingSectionBefore.OrderIndependentCompareEqual(PersistentValues(ExistingSection)));
				}
			}
		}
		Check(TEXT("before-proposed-close"));
		if (bOverridePlayback) FFileHelper::LoadFileToString(BeforeCloseIni, *GEditorPerProjectIni);
		Observation.Stage = TEXT("proposed-action:closing-proposed-context");
		ProposedContext->Close();
		Check(TEXT("after-proposed-close"));
		if (bOverridePlayback) DiagnoseIniDelta(*this, BeforeCloseIni, TEXT("after-proposed-close"));
		return true;
	}));
	return true;
}

class FUeAgentT85ProposedIsolationVariantBase : public FUeAgentT85SequenceProposedNativeActionIsolationTest
{
public:
	FUeAgentT85ProposedIsolationVariantBase(const FString& Name, bool)
		: FUeAgentT85SequenceProposedNativeActionIsolationTest(Name) {}
};

IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FUeAgentT85SequenceProposedPlaybackOverrideIsolationTest,
	FUeAgentT85ProposedIsolationVariantBase,
	"UAI.Root.T85.LevelSequence.NativeTrackMenu.ProposedPlaybackOverrideIsolation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85SequenceProposedPlaybackOverrideIsolationTest::RunTest(const FString&)
{
	return FUeAgentT85SequenceProposedNativeActionIsolationTest::RunTest(TEXT("OverridePlayback"));
}

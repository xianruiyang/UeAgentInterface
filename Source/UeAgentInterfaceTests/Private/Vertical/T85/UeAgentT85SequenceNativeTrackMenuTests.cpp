#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Components/PointLightComponent.h"
#include "Components/SplineComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSocket.h"
#include "ActorTreeItem.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "SSceneOutliner.h"
#include "SSocketChooser.h"
#include "Sections/MovieScene3DAttachSection.h"
#include "Sections/MovieScene3DPathSection.h"
#include "Tracks/MovieScene3DAttachTrack.h"
#include "Tracks/MovieScene3DPathTrack.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "DomainAdapters/TimelineAnimation/UeAgentTimelineAnimationNative.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "GenericPlatform/GenericWindow.h"
#include "Engine/PointLight.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBox.h"
#include "ISequencer.h"
#include "ISequencerModule.h"
#include "LevelSequence.h"
#include "Misc/PackageName.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "MovieScene.h"
#include "MovieScenePossessable.h"
#include "MVVM/ObjectBindingModelStorageExtension.h"
#include "MVVM/ViewModels/ObjectBindingModel.h"
#include "MVVM/ViewModels/SequenceModel.h"
#include "MVVM/ViewModels/SequencerEditorViewModel.h"
#include "ObjectTools.h"
#include "PreviewScene.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Sections/MovieSceneEventTriggerSection.h"
#include "Tracks/MovieSceneEventTrack.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Tracks/MovieScenePropertyTrack.h"
#include "TestSupport/UeAgentPublicCommandTestHarness.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"
#include "Vertical/T85/UeAgentT85FixtureLifecycle.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SMenuAnchor.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SWindow.h"

namespace
{
	TSharedPtr<SMultiBoxWidget> FindNativeMenuBox(const TSharedRef<SWidget>& Root)
	{
		TArray<TSharedRef<SWidget>> Pending{Root};
		while (!Pending.IsEmpty())
		{
			const auto Widget = Pending.Pop(EAllowShrinking::No);
			if (Widget->GetType() == TEXT("SMultiBoxWidget"))
				return StaticCastSharedRef<SMultiBoxWidget>(Widget);
			FChildren* Children = Widget->GetChildren();
			for (int32 Index = 0; Index < Children->Num(); ++Index) Pending.Add(Children->GetChildAt(Index));
		}
		return nullptr;
	}

	TArray<FString> ReadNativeMenuTexts(const TSharedRef<SWidget>& Root)
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

	bool HasExactBoundObject(ISequencer& Sequencer, const FGuid& Guid, UObject* Expected)
	{
		const auto Objects = Sequencer.FindBoundObjects(Guid, MovieSceneSequenceID::Root);
		return Objects.Num() == 1 && Objects[0].Get() == Expected;
	}
}

namespace
{
	class SNativeMenuProbeHost : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SNativeMenuProbeHost) {} SLATE_END_ARGS()
		void Construct(const FArguments&) {}
		void SetContent(const TSharedRef<SWidget>& Content) { ChildSlot[Content]; }
		FPopupMethodReply OnQueryPopupMethod() const override
		{ return FPopupMethodReply::UseMethod(EPopupMethod::UseCurrentWindow); }
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85SequenceNativeMenuTreeTest,
	"UAI.Root.T85.LevelSequence.NativeTrackMenu.Tree",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85SequenceNativeMenuTreeTest::RunTest(const FString&)
{
	using namespace UE::Sequencer;
	FPreviewScene Preview(FPreviewScene::ConstructionValues().SetCreateDefaultLighting(false).SetCreatePhysicsScene(false));
	APointLight* Actor = Preview.GetWorld()->SpawnActor<APointLight>();
	UPointLightComponent* Component = Actor ? Cast<UPointLightComponent>(Actor->GetLightComponent()) : nullptr;
	if (!TestNotNull(TEXT("isolated real component"), Component)) return false;
	ULevelSequence* Sequence = NewObject<ULevelSequence>(GetTransientPackage(), NAME_None, RF_Transactional);
	Sequence->Initialize();
	ISequencerModule& Module = FModuleManager::LoadModuleChecked<ISequencerModule>(TEXT("Sequencer"));
	FModuleManager::Get().LoadModuleChecked<IModuleInterface>(TEXT("MovieSceneTools"));
	FSequencerInitParams Init;
	Init.RootSequence = Sequence; Init.PlaybackContext = Preview.GetWorld();
	Init.ViewParams.UniqueName = TEXT("UaiT85NativeMenuTree"); Init.bEditWithinLevelEditor = false;
	const auto Sequencer = Module.CreateSequencer(Init);
	ON_SCOPE_EXIT { Sequencer->Close(); };
	const FGuid BindingGuid = Sequencer->GetHandleToObject(Component, true);
	auto* Storage = Sequencer->GetViewModel()->GetRootSequenceModel()->CastDynamic<FObjectBindingModelStorageExtension>();
	const auto BindingModel = Storage ? Storage->FindModelForObjectBinding(BindingGuid) : nullptr;
	if (!TestTrue(TEXT("real native binding model"), BindingModel.IsValid())) return false;
	IConsoleVariable* Nested = IConsoleManager::Get().FindConsoleVariable(TEXT("Sequencer.ObjectBinding.ShowNestedProperties"));
	if (!TestNotNull(TEXT("native nested-property presentation setting"), Nested)) return false;
	const int32 BeforeNested = Nested->GetInt();
	const auto NestedPriority = static_cast<EConsoleVariableFlags>(Nested->GetFlags() & ECVF_SetByMask);
	ON_SCOPE_EXIT { Nested->Set(BeforeNested, NestedPriority); };
	// This changes native presentation only. All contents still come from the
	// original binding menu's own submenu delegates.
	Nested->Set(1, NestedPriority);
	const auto Menu = BindingModel->GetAddTrackMenuContent();
	const auto RootBox = FindNativeMenuBox(Menu);
	if (!TestTrue(TEXT("native menu MultiBox"), RootBox.IsValid())) return false;
	const int32 WindowsBefore = FSlateApplication::Get().GetTopLevelWindows().Num();
	const auto Host = SNew(SNativeMenuProbeHost);
	const auto Window = SNew(SWindow).Title(FText::FromString(TEXT("UAI owned hidden menu probe")))
		.ClientSize(FVector2D(800, 800)).SupportsMaximize(false).SupportsMinimize(false)[Host];
	FSlateApplication::Get().AddWindow(Window, false);
	ON_SCOPE_EXIT { FSlateApplication::Get().DestroyWindowImmediately(Window); };
	auto HasNoVisibleNativeWindow = [&] {
		const auto Native = Window->GetNativeWindow();
		// NullRHI uses FGenericWindow: IsVisible() returns a constant true even
		// though GetOSWindowHandle() is null and no OS window exists.
		return Native.IsValid() && (!Native->GetOSWindowHandle() || !Native->IsVisible());
	};
	if (!TestTrue(TEXT("owned host has no visible OS window"), HasNoVisibleNativeWindow())) return false;
	AddInfo(FString::Printf(TEXT("NativeMenuTree host_os_window=%d native_visible=%d"), Window->GetNativeWindow()->GetOSWindowHandle() != nullptr, Window->GetNativeWindow()->IsVisible()));
	struct FLeaf { FString Path; FString Label; TSharedPtr<const FMultiBlock> Block; int32 Depth; };
	TArray<FLeaf> Leaves;
	TMap<FString, TArray<FString>> PathsByLabel;
	int32 Blocks = 0, Anchors = 0, Opened = 0, NonMenuContent = 0, UnboundBlocks = 0;
	TFunction<bool(TSharedRef<SMultiBoxWidget>, const FString&, int32)> Visit;
	Visit = [&](TSharedRef<SMultiBoxWidget> Box, const FString& Prefix, int32 Depth) {
		if (Depth > 12) { AddError(TEXT("native menu probe depth bound reached")); return false; }
		int32 Index = 0;
		for (const auto& Block : Box->GetMultiBox()->GetBlocks())
		{
			if (++Blocks > 256) { AddError(TEXT("native menu probe block bound reached")); return false; }
			const auto Row = Block->MakeWidget(Box, EMultiBlockLocation::None, true, nullptr)->AsWidget();
			const auto Texts = ReadNativeMenuTexts(Row);
			const FString Label = FString::Join(Texts, TEXT(" | "));
			const FString Path = Prefix.IsEmpty() ? Label : Prefix + TEXT(" / ") + Label;
			TArray<TSharedRef<SMenuAnchor>> RowAnchors;
			TArray<TSharedRef<SWidget>> Pending{Row};
			while (!Pending.IsEmpty())
			{
				const auto Widget = Pending.Pop(EAllowShrinking::No);
				if (Widget->GetType() == TEXT("SMenuAnchor")) RowAnchors.Add(StaticCastSharedRef<SMenuAnchor>(Widget));
				FChildren* Children = Widget->GetChildren();
				for (int32 Child = 0; Child < Children->Num(); ++Child) Pending.Add(Children->GetChildAt(Child));
			}
			const bool bDirect = Block->GetDirectActions().IsBound();
			AddInfo(FString::Printf(TEXT("NativeMenuTree row=%d depth=%d type=%d hook=%s label=%s direct=%d anchors=%d"),
				Index++, Depth, static_cast<int32>(Block->GetType()), *Block->GetExtensionHook().ToString(), *Path, bDirect, RowAnchors.Num()));
			if (bDirect)
			{
				Leaves.Add({Path, Label, Block, Depth}); PathsByLabel.FindOrAdd(Label).Add(Path);
			}
			else ++UnboundBlocks;
			for (const auto& Anchor : RowAnchors)
			{
				++Anchors;
				Host->SetContent(Row); Window->SlatePrepass();
				Anchor->SetIsOpen(true, false);
				if (!TestTrue(TEXT("native anchor opens using hidden current window"), Anchor->IsOpen()
					&& Anchor->GetMenuWindow() == Window && HasNoVisibleNativeWindow()
					&& FSlateApplication::Get().GetTopLevelWindows().Num() == WindowsBefore + 1)) return false;
				++Opened;
				const auto ChildBox = FindNativeMenuBox(Anchor);
				Anchor->SetIsOpen(false, false);
				if (!ChildBox) { ++NonMenuContent; AddInfo(TEXT("NativeMenuTree unexpanded_custom_content=") + Path); continue; }
				if (!Visit(ChildBox.ToSharedRef(), Path, Depth + 1)) return false;
			}
		}
		return true;
	};
	if (!Visit(RootBox.ToSharedRef(), FString(), 0)) return false;
	int32 DuplicateLabels = 0;
	for (const auto& Pair : PathsByLabel)
		if (Pair.Value.Num() > 1)
		{ ++DuplicateLabels; AddInfo(TEXT("NativeMenuTree duplicate_label=") + Pair.Key + TEXT(" paths=") + FString::Join(Pair.Value, TEXT(" || "))); }
	// These are the native EventTrackEditor localization identities. They select
	// an existing menu leaf; its original action remains owned by the live Sequencer.
	const FString DesiredPath = NSLOCTEXT("FEventTrackEditor", "AddEventTrack_ObjectBinding", "Event").ToString()
		+ TEXT(" / ") + NSLOCTEXT("FEventTrackEditor", "AddNewTriggerSection", "Trigger").ToString();
	const auto Matches = Leaves.FilterByPredicate([&](const FLeaf& Leaf) { return Leaf.Depth == 1 && Leaf.Path == DesiredPath && Leaf.Block->GetDirectActions().CanExecute(); });
	if (!TestEqual(TEXT("native Event Trigger submenu leaf uniquely selected before execution"), Matches.Num(), 1)) return false;
	const FLeaf* Selected = &Matches[0];
	if (Selected)
	{
		const auto* Before = Sequence->GetMovieScene()->FindBinding(BindingGuid);
		if (!TestTrue(TEXT("real target binding has no tracks before selected nested action"), Before && Before->GetTracks().IsEmpty())) return false;
		if (!TestTrue(TEXT("same selected native nested leaf action executes"), Selected->Block->GetDirectActions().Execute())) return false;
		const auto* After = Sequence->GetMovieScene()->FindBinding(BindingGuid);
		const auto* Track = After && After->GetTracks().Num() == 1 ? Cast<UMovieSceneEventTrack>(After->GetTracks()[0]) : nullptr;
		if (!TestTrue(TEXT("preselected nested action creates EventTrack on the original binding"), Track
			&& HasExactBoundObject(*Sequencer, BindingGuid, Component))) return false;
		const auto* Section = Track->GetAllSections().Num() == 1 ? Cast<UMovieSceneEventTriggerSection>(Track->GetAllSections()[0]) : nullptr;
		if (!TestTrue(TEXT("preselected Trigger action creates exactly one TriggerSection with native unbounded range"), Section
			&& Section->GetRange().GetLowerBound().IsOpen() && Section->GetRange().GetUpperBound().IsOpen())) return false;
		TestTrue(TEXT("binding action creates no root track"), Sequence->GetMovieScene()->GetTracks().IsEmpty());
		for (const auto& Binding : Sequence->GetMovieScene()->GetBindings())
			if (Binding.GetObjectGuid() != BindingGuid) TestTrue(TEXT("other bindings gain no tracks"), Binding.GetTracks().IsEmpty());
		AddInfo(FString::Printf(TEXT("NativeMenuTree selected=%s track_class=%s section_class=%s same_action=1 real_binding=1"),
			*Selected->Path, *Track->GetClass()->GetName(), *Section->GetClass()->GetName()));
	}
	AddInfo(FString::Printf(TEXT("NativeMenuTree blocks=%d direct_leaves=%d anchors=%d opened=%d custom_content=%d unbound_blocks=%d duplicate_labels=%d selected_nested=%d hidden_host=1"),
		Blocks, Leaves.Num(), Anchors, Opened, NonMenuContent, UnboundBlocks, DuplicateLabels, Selected != nullptr));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85SequenceNativeTrackMenuTest,
	"UAI.Root.T85.LevelSequence.NativeTrackMenu.FloatProperty",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85SequenceNativeTrackMenuTest::RunTest(const FString&)
{
	using namespace UE::Sequencer;
	const FString Name = TEXT("LS_NativeTrack_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = TEXT("/Game/UAI/Transient/T85SequenceNativeTrack/") + Name;
	const FString ObjectPath = PackageName + TEXT(".") + Name;
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	FPreviewScene Preview(FPreviewScene::ConstructionValues().SetCreateDefaultLighting(false).SetCreatePhysicsScene(false));
	UWorld* World = Preview.GetWorld();
	APointLight* Actor = World->SpawnActor<APointLight>();
	if (!TestNotNull(TEXT("isolated real light actor"), Actor)) return false;
	UPointLightComponent* Component = Cast<UPointLightComponent>(Actor->GetLightComponent());
	if (!TestNotNull(TEXT("isolated real light component"), Component)) return false;
	constexpr float ExpectedIntensity = 1234.5f;
	Component->SetIntensity(ExpectedIntensity);
	UPackage* Package = CreatePackage(*PackageName);
	ULevelSequence* Sequence = NewObject<ULevelSequence>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional);
	Sequence->Initialize();
	FAssetRegistryModule::AssetCreated(Sequence);
	ON_SCOPE_EXIT {
		if (UObject* Asset = LoadObject<UObject>(nullptr, *ObjectPath))
			TestEqual(TEXT("native-menu fixture deleted through UE"), ObjectTools::DeleteObjects({Asset}, false), 1);
		UE::UAI::T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, PackageName);
	};
	const FFloatProperty* Property = FindFProperty<FFloatProperty>(Component->GetClass(), TEXT("Intensity"));
	if (!TestNotNull(TEXT("preselected native float property exists"), Property)) return false;
	const FString PropertyName = Property->GetName();
	const FString ReadableName = Property->GetDisplayNameText().ToString();
	const int32 WindowsBefore = FSlateApplication::Get().GetTopLevelWindows().Num();
	ISequencerModule& Module = FModuleManager::LoadModuleChecked<ISequencerModule>(TEXT("Sequencer"));
	FModuleManager::Get().LoadModuleChecked<IModuleInterface>(TEXT("MovieSceneTools"));
	auto CreateSequencer = [&](ULevelSequence* Root) {
		FSequencerInitParams Init;
		Init.RootSequence = Root;
		Init.PlaybackContext = World;
		Init.ViewParams.UniqueName = TEXT("UaiT85NativeTrackMenu");
		Init.bEditWithinLevelEditor = false;
		return Module.CreateSequencer(Init);
	};
	FGuid ComponentGuid, ActorGuid;
	TArray<FFrameNumber> ExpectedTimes;
	TArray<FMovieSceneFloatValue> ExpectedValues;
	TOptional<float> ExpectedDefault;
	int32 NativeMenuActions = 0;
	bool bAutoDefaults = false;
	{
		const auto Sequencer = CreateSequencer(Sequence);
		const bool bOriginalInfinite = Sequencer->GetInfiniteKeyAreas();
		ON_SCOPE_EXIT { Sequencer->SetInfiniteKeyAreas(bOriginalInfinite); Sequencer->Close(); };
		ComponentGuid = Sequencer->GetHandleToObject(Component, true);
		ActorGuid = Sequencer->GetHandleToObject(Actor, false);
		const auto* Possessable = Sequence->GetMovieScene()->FindPossessable(ComponentGuid);
		if (!TestTrue(TEXT("native binding connects component and parent actor"), ComponentGuid.IsValid() && ActorGuid.IsValid()
			&& Possessable && Possessable->GetParent() == ActorGuid
			&& HasExactBoundObject(*Sequencer, ComponentGuid, Component) && HasExactBoundObject(*Sequencer, ActorGuid, Actor))) return false;
		const auto* BeforeBinding = Sequence->GetMovieScene()->FindBinding(ComponentGuid);
		if (!TestTrue(TEXT("target component has no track before menu action"), BeforeBinding && BeforeBinding->GetTracks().IsEmpty())) return false;
		const auto RootModel = Sequencer->GetViewModel()->GetRootSequenceModel();
		auto* Storage = RootModel->CastDynamic<FObjectBindingModelStorageExtension>();
		const auto BindingModel = Storage ? Storage->FindModelForObjectBinding(ComponentGuid) : nullptr;
		if (!TestTrue(TEXT("public binding model exists"), BindingModel.IsValid())) return false;
		const auto Menu = BindingModel->GetAddTrackMenuContent();
		const auto MultiBox = FindNativeMenuBox(Menu);
		if (!TestTrue(TEXT("public native Add Track menu exposes MultiBox"), MultiBox.IsValid())) return false;
		TArray<TSharedRef<const FMultiBlock>> Matches;
		for (const auto& Block : MultiBox->GetMultiBox()->GetBlocks())
		{
			if (!Block->GetDirectActions().IsBound()) continue;
			++NativeMenuActions;
			// Use the block's public widget interface for its actual text. No private
			// menu-entry cast, copied property filtering, or substitute action.
			const auto BlockWidget = Block->MakeWidget(MultiBox.ToSharedRef(), EMultiBlockLocation::None, true, nullptr);
			if (ReadNativeMenuTexts(BlockWidget->AsWidget()).Contains(ReadableName)) Matches.Add(Block);
		}
		if (!TestEqual(TEXT("exact readable property candidate uniquely selects native action"), Matches.Num(), 1))
		{
			AddError(FString::Printf(TEXT("wanted=%s native_direct_actions=%d"), *ReadableName, NativeMenuActions));
			return false;
		}
		bAutoDefaults = Sequencer->GetAutoSetTrackDefaults();
		if (!TestTrue(TEXT("same native Add Track action executes"), Matches[0]->GetDirectActions().Execute())) return false;
		const auto* AfterBinding = Sequence->GetMovieScene()->FindBinding(ComponentGuid);
		if (!TestTrue(TEXT("exact target binding gains one track"), AfterBinding && AfterBinding->GetTracks().Num() == 1)) return false;
		auto* Track = Cast<UMovieSceneFloatTrack>(AfterBinding->GetTracks()[0]);
		if (!TestTrue(TEXT("native action creates the selected Float property track"), Track
			&& Track->GetPropertyName() == Property->GetFName() && Track->GetPropertyPath().ToString() == PropertyName
			&& Track->GetAllSections().Num() == 1)) return false;
		const auto* Section = Cast<UMovieSceneFloatSection>(Track->GetAllSections()[0]);
		if (!TestNotNull(TEXT("native menu section exists"), Section)) return false;
		AddInfo(FString::Printf(TEXT("NativeMenu initial_infinite_setting=%d lower_open=%d upper_open=%d"),
			bOriginalInfinite, Section->GetRange().GetLowerBound().IsOpen(), Section->GetRange().GetUpperBound().IsOpen()));
		if (Section->HasStartFrame() || Section->HasEndFrame())
		{
			Sequence->GetMovieScene()->RemoveTrack(*Track);
			Sequencer->SetInfiniteKeyAreas(true);
			if (!TestTrue(TEXT("same native action recreates target under explicit infinite setting"), Matches[0]->GetDirectActions().Execute())) return false;
			AfterBinding = Sequence->GetMovieScene()->FindBinding(ComponentGuid);
			Track = AfterBinding && AfterBinding->GetTracks().Num() == 1 ? Cast<UMovieSceneFloatTrack>(AfterBinding->GetTracks()[0]) : nullptr;
			Section = Track && Track->GetAllSections().Num() == 1 ? Cast<UMovieSceneFloatSection>(Track->GetAllSections()[0]) : nullptr;
		}
		if (!TestTrue(TEXT("same menu action creates genuinely unbounded section"), Section
			&& Section->GetRange().GetLowerBound().IsOpen() && Section->GetRange().GetUpperBound().IsOpen())) return false;
		const auto* Channel = Section ? Section->GetChannelProxy().GetChannel<FMovieSceneFloatChannel>(0) : nullptr;
		float Evaluated = 0.f;
		if (!TestTrue(TEXT("new native channel evaluates the bound object's original property value"), Channel
			&& Channel->Evaluate(FFrameTime(0), Evaluated) && Evaluated == ExpectedIntensity)) return false;
		ExpectedDefault = Channel->GetDefault();
		ExpectedTimes.Append(Channel->GetData().GetTimes());
		ExpectedValues.Append(Channel->GetData().GetValues());
		if (!TestTrue(TEXT("native default/key policy stores the selected value"), bAutoDefaults
			? ExpectedDefault.IsSet() && *ExpectedDefault == ExpectedIntensity : !ExpectedValues.IsEmpty())) return false;
		TestTrue(TEXT("menu action retains exact component and actor bindings"),
			HasExactBoundObject(*Sequencer, ComponentGuid, Component) && HasExactBoundObject(*Sequencer, ActorGuid, Actor));
	}
	TestEqual(TEXT("native Sequencer and menu created no top-level window"), FSlateApplication::Get().GetTopLevelWindows().Num(), WindowsBefore);
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	if (!TestTrue(TEXT("menu-created sequence saved"), UPackage::SavePackage(Package, Sequence, *Filename, SaveArgs))) return false;
	TWeakObjectPtr<ULevelSequence> OldSequence = Sequence;
	Sequence = nullptr; Package = nullptr;
	FString Error;
	if (!TestTrue(TEXT("menu-created sequence independently unloads"), UE::UAI::T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)))
	{ AddError(Error); return false; }
	TestFalse(TEXT("old sequence object invalid after unload"), OldSequence.IsValid(true, true));
	Sequence = LoadObject<ULevelSequence>(nullptr, *ObjectPath);
	if (!TestNotNull(TEXT("menu-created sequence reloads"), Sequence)) return false;
	{
		const auto Sequencer = CreateSequencer(Sequence);
		ON_SCOPE_EXIT { Sequencer->Close(); };
		const auto* Possessable = Sequence->GetMovieScene()->FindPossessable(ComponentGuid);
		TestTrue(TEXT("persisted component locator resolves to the original real object and parent actor"),
			Possessable && Possessable->GetParent() == ActorGuid && HasExactBoundObject(*Sequencer, ComponentGuid, Component)
			&& HasExactBoundObject(*Sequencer, ActorGuid, Actor));
		const auto* Binding = Sequence->GetMovieScene()->FindBinding(ComponentGuid);
		const auto* Track = Binding && Binding->GetTracks().Num() == 1 ? Cast<UMovieSceneFloatTrack>(Binding->GetTracks()[0]) : nullptr;
		if (!TestTrue(TEXT("reload retains selected native Float track and property path"), Track
			&& Track->GetPropertyName().ToString() == PropertyName && Track->GetPropertyPath().ToString() == PropertyName
			&& Track->GetAllSections().Num() == 1)) return false;
		const auto* Section = Cast<UMovieSceneFloatSection>(Track->GetAllSections()[0]);
		const auto* Channel = Section ? Section->GetChannelProxy().GetChannel<FMovieSceneFloatChannel>(0) : nullptr;
		if (!TestNotNull(TEXT("reloaded native Float channel"), Channel)) return false;
		TArray<FFrameNumber> Times; Times.Append(Channel->GetData().GetTimes());
		TArray<FMovieSceneFloatValue> Values; Values.Append(Channel->GetData().GetValues());
		TestTrue(TEXT("reload preserves all native keys interpolation tangents and default"),
			Times == ExpectedTimes && Values == ExpectedValues && Channel->GetDefault() == ExpectedDefault);
	}
	TestFalse(TEXT("reloaded sequence clean"), Sequence->GetOutermost()->IsDirty());
	// The target remains the section created by the native menu. A separate root
	// track gives the public mutations a fixed, nontrivial non-target oracle.
	auto* OtherTrack = Sequence->GetMovieScene()->AddTrack<UMovieSceneFloatTrack>();
	OtherTrack->SetPropertyNameAndPath(TEXT("Unrelated"), TEXT("Unrelated"));
	auto* OtherSection = Cast<UMovieSceneFloatSection>(OtherTrack->CreateNewSection());
	OtherSection->SetRange(TRange<FFrameNumber>(FFrameNumber(-500), FFrameNumber(500)));
	OtherTrack->AddSection(*OtherSection);
	OtherSection->GetChannel().SetDefault(17.f);
	OtherSection->GetChannel().AddCubicKey(FFrameNumber(12), 19.f);
	// Finish native Sequencer initialization of the entire fixture before fixing
	// the oracle; its playback-range policy runs when a Sequencer is opened.
	{
		const auto Sequencer = CreateSequencer(Sequence); ON_SCOPE_EXIT { Sequencer->Close(); };
	}
	auto CapturePersistent = [](UObject& Object) {
		TMap<FString, FString> Result;
		for (TFieldIterator<FProperty> It(Object.GetClass()); It; ++It)
		{
			if (It->HasAnyPropertyFlags(CPF_Transient)) continue;
			FString Value; It->ExportText_InContainer(0, Value, &Object, nullptr, &Object, PPF_None);
			Result.Add(It->GetName(), Value);
		}
		return Result;
	};
	const auto OtherBefore = CapturePersistent(*OtherSection);
	const auto OtherTrackBefore = CapturePersistent(*OtherTrack);
	if (!UPackage::SavePackage(Sequence->GetOutermost(), Sequence, *Filename, SaveArgs)) return false;
	using namespace UE::UAI;
	using namespace DomainAdapters::TimelineAnimation;
	TSharedPtr<FJsonObject> Expected;
	FUeAgentTimelineAnimationExecutionError NativeError;
	if (!TryReadLevelSequenceStructure(*Sequence, ObjectPath, Expected, NativeError))
	{ AddError(NativeError.Code); return false; }
	TSharedPtr<FJsonObject> ExpectedSection;
	for (const auto& BindingValue : Expected->GetArrayField(TEXT("bindings")))
		for (const auto& TrackValue : BindingValue->AsObject()->GetArrayField(TEXT("tracks")))
			if (TrackValue->AsObject()->GetStringField(TEXT("property_path")) == PropertyName)
				ExpectedSection = TrackValue->AsObject()->GetArrayField(TEXT("sections"))[0]->AsObject();
	if (!TestTrue(TEXT("fixed native target found before ordinary mutation"), ExpectedSection.IsValid())) return false;
	const FString Workspace = FPaths::ProjectSavedDir() / TEXT("Automation/UAI/T85/NativeOpenRange") / Name;
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Workspace, false, true); };
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) return false;
	FString TargetFile, TargetSectionKey;
	for (int32 Phase = 0; Phase < 5; ++Phase)
	{
		const auto ExportArgs = MakeShared<FJsonObject>();
		ExportArgs->SetStringField(TEXT("target"), ObjectPath);
		ExportArgs->SetStringField(TEXT("workspace_path"), Workspace);
		ExportArgs->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
		const auto Exported = Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch,
			TEXT("content_graph.export_workspace"), ExportArgs, Name, {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
		if (!Exported.IsOk()) { AddError(Exported.GetErrorJson()); return false; }
		TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *Workspace, TEXT("*.hjson"), true, false);
		TSharedPtr<FJsonObject> TargetDocument, PublicSection;
		for (const FString& File : Files)
		{
			if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\")) || (!TargetFile.IsEmpty() && File != TargetFile)) continue;
			FString Text; TSharedPtr<FJsonObject> Document;
			if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Document, Error)) return false;
			TArray<TSharedPtr<FJsonValue>> Pending{MakeShared<FJsonValueObject>(Document)};
			while (!Pending.IsEmpty())
			{
				const auto Value = Pending.Pop(EAllowShrinking::No);
				if (Value->Type == EJson::Array) Pending.Append(Value->AsArray());
				else if (Value->Type == EJson::Object)
				{
					const auto Object = Value->AsObject();
					const TSharedPtr<FJsonObject>* Sections = nullptr;
					if (Object->TryGetObjectField(TEXT("sections"), Sections))
						for (const auto& Pair : (*Sections)->Values)
						{
							bool bTarget = !TargetFile.IsEmpty() && Pair.Key == TargetSectionKey;
							if (TargetFile.IsEmpty())
								for (const auto& Key : Pair.Value->AsObject()->GetObjectField(TEXT("keys"))->Values)
									bTarget |= Key.Value->AsObject()->GetStringField(TEXT("value_lexeme")) == TEXT("1234.5");
							if (bTarget) { TargetFile = File; TargetSectionKey = Pair.Key; TargetDocument = Document; PublicSection = Pair.Value->AsObject(); }
						}
					for (const auto& Field : Object->Values) Pending.Add(Field.Value);
				}
			}
		}
		if (!TestTrue(TEXT("preselected menu section appears in ordinary collection"), PublicSection.IsValid())) return false;
		if (Phase == 0)
		{
			for (auto& Value : ExpectedValues) Value.Value = 2222.25f;
			TestFalse(TEXT("ordinary native open lower has no point"), PublicSection->GetObjectField(TEXT("range"))->GetObjectField(TEXT("lower"))->HasField(TEXT("point")));
			for (const auto& Key : PublicSection->GetObjectField(TEXT("keys"))->Values) Key.Value->AsObject()->SetStringField(TEXT("value_lexeme"), TEXT("2222.25"));
			for (const auto& Key : ExpectedSection->GetArrayField(TEXT("keys"))) Key->AsObject()->SetStringField(TEXT("value_lexeme"), TEXT("2222.25"));
		}
		else
		{
			const auto Range = MakeShared<FJsonObject>();
			const auto ExpectedRange = ExpectedSection->GetObjectField(TEXT("range"));
			for (const FString Side : {FString(TEXT("lower")), FString(TEXT("upper"))})
			{
				const bool bLower = Side == TEXT("lower");
				const FString Kind = Phase == 4 || (Phase == 3 && bLower) ? TEXT("open")
					: Phase == 2 ? (bLower ? TEXT("exclusive") : TEXT("inclusive")) : (bLower ? TEXT("inclusive") : TEXT("exclusive"));
				const auto Bound = MakeShared<FJsonObject>(); Bound->SetStringField(TEXT("kind"), Kind);
				const auto ExpectedBound = MakeShared<FJsonObject>(); ExpectedBound->SetStringField(TEXT("kind"), Kind);
				if (Kind != TEXT("open"))
				{
					TSharedPtr<FJsonObject> Point;
					if (Phase == 1)
					{
						Point = MakeShared<FJsonObject>(); Point->Values = Expected->GetObjectField(TEXT("playback_range"))->GetObjectField(Side)->GetObjectField(TEXT("point"))->Values;
						Point->SetStringField(TEXT("tick"), bLower ? TEXT("-100") : TEXT("100"));
						const auto VisiblePoint = MakeShared<FJsonObject>(); VisiblePoint->SetStringField(TEXT("tick"), Point->GetStringField(TEXT("tick")));
						Bound->SetObjectField(TEXT("point"), VisiblePoint);
					}
					else Point = ExpectedRange->GetObjectField(Side)->GetObjectField(TEXT("point"));
					ExpectedBound->SetObjectField(TEXT("point"), Point);
				}
				Range->SetObjectField(Side, Bound); ExpectedRange->SetObjectField(Side, ExpectedBound);
			}
			PublicSection->SetObjectField(TEXT("range"), Range);
		}
		FString Text;
		if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(TargetDocument.ToSharedRef(), Text, Error)
			|| !FFileHelper::SaveStringToFile(Text, *TargetFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
		const auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Workspace);
		const auto ImportArgs = MakeShared<FJsonObject>(); ImportArgs->SetStringField(TEXT("target"), ObjectPath); ImportArgs->SetObjectField(TEXT("source"), Source);
		OldSequence = Sequence; Sequence = nullptr; OtherTrack = nullptr; OtherSection = nullptr;
		const auto Imported = Tests::PublicCommand::SubmitActiveCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), ImportArgs, Name);
		if (!Imported.IsOk()) { AddError(Imported.GetErrorJson()); return false; }
		if (!T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)) { AddError(Error); return false; }
		TestFalse(TEXT("ordinary mutation old sequence independently exits"), OldSequence.IsValid(true, true));
		Sequence = LoadObject<ULevelSequence>(nullptr, *ObjectPath);
		TSharedPtr<FJsonObject> Readback; FString ActualText, ExpectedText;
		if (!Sequence || !TryReadLevelSequenceStructure(*Sequence, ObjectPath, Readback, NativeError)
			|| !CommandContract::Json::TryCanonicalize(Readback, ActualText, Error)
			|| !CommandContract::Json::TryCanonicalize(Expected, ExpectedText, Error)) return false;
		TestEqual(TEXT("full fixed native structure and bound semantics persist"), ActualText, ExpectedText);
		const auto* TargetBinding = Sequence->GetMovieScene()->FindBinding(ComponentGuid);
		const auto* TargetNativeSection = TargetBinding && TargetBinding->GetTracks().Num() == 1
			? Cast<UMovieSceneFloatSection>(TargetBinding->GetTracks()[0]->GetAllSections()[0]) : nullptr;
		if (!TestNotNull(TEXT("fixed native section persists after ordinary edit"), TargetNativeSection)) return false;
		TArray<FFrameNumber> ActualTimes; ActualTimes.Append(TargetNativeSection->GetChannel().GetData().GetTimes());
		TArray<FMovieSceneFloatValue> ActualValues; ActualValues.Append(TargetNativeSection->GetChannel().GetData().GetValues());
		TestTrue(TEXT("target key times all interpolation tangents and default remain exact"),
			ActualTimes == ExpectedTimes && ActualValues == ExpectedValues && TargetNativeSection->GetChannel().GetDefault() == ExpectedDefault);
		const auto ActualOtherTrack = CapturePersistent(*Sequence->GetMovieScene()->GetTracks()[0]);
		const auto ActualOther = CapturePersistent(*Sequence->GetMovieScene()->GetTracks()[0]->GetAllSections()[0]);
		TestTrue(TEXT("full unrelated track and channel persist"), OtherTrackBefore.OrderIndependentCompareEqual(ActualOtherTrack)
			&& OtherBefore.OrderIndependentCompareEqual(ActualOther));
		for (const auto& Field : OtherBefore)
			if (ActualOther.FindRef(Field.Key) != Field.Value) AddError(TEXT("non-target section field ") + Field.Key + TEXT(" before=") + Field.Value + TEXT(" after=") + ActualOther.FindRef(Field.Key));
		for (const auto& Field : OtherTrackBefore)
			if (ActualOtherTrack.FindRef(Field.Key) != Field.Value) AddError(TEXT("non-target track field ") + Field.Key + TEXT(" before=") + Field.Value + TEXT(" after=") + ActualOtherTrack.FindRef(Field.Key));
		{
			const auto Sequencer = CreateSequencer(Sequence); ON_SCOPE_EXIT { Sequencer->Close(); };
			TestTrue(TEXT("ordinary edit preserves real component and actor binding"), HasExactBoundObject(*Sequencer, ComponentGuid, Component) && HasExactBoundObject(*Sequencer, ActorGuid, Actor));
		}
		TestFalse(TEXT("ordinary range reload is clean"), Sequence->GetOutermost()->IsDirty());
		if (HasAnyErrors()) return false;
	}
	AddInfo(TEXT("NativeMenuOpenRange public_key_edit=1 range_phases=5 symmetric_bound_kinds=1 full_non_target=1 independent_reload=1 real_binding=1"));
	TestEqual(TEXT("reloaded headless Sequencer adds no top-level window"), FSlateApplication::Get().GetTopLevelWindows().Num(), WindowsBefore);
	AddInfo(FString::Printf(TEXT("NativeTrackMenu candidate=%s property=%s menu_actions=%d same_action=1 before_tracks=0 after_tracks=1 real_component_binding=1 parent_actor_binding=1 saved_unloaded_reloaded=1 auto_defaults=%d keys=%d default=%s no_window=1"),
		*ReadableName, *PropertyName, NativeMenuActions, bAutoDefaults, ExpectedTimes.Num(), ExpectedDefault.IsSet() ? *FString::SanitizeFloat(*ExpectedDefault) : TEXT("unset")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85SequenceNativeActorPickerTest,
	"UAI.Root.T85.LevelSequence.NativeTrackMenu.ActorPicker",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85SequenceNativeActorPickerTest::RunTest(const FString&)
{
	using namespace UE::Sequencer;
	// The original native picker uses the LevelEditor world. Do not substitute a
	// newly constructed picker (which would lose its filter and creation delegate).
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!TestNotNull(TEXT("native picker editor world"), World)) return false;
	const bool bWorldDirtyBefore = World->GetOutermost()->IsDirty();
	TSet<AActor*> WorldActorsBefore;
	for (TActorIterator<AActor> It(World); It; ++It) WorldActorsBefore.Add(*It);
	FActorSpawnParameters Spawn;
	Spawn.ObjectFlags = RF_Transient;
	APointLight* Child = World->SpawnActor<APointLight>(Spawn);
	APointLight* Parent = World->SpawnActor<APointLight>(Spawn);
	APointLight* Path = World->SpawnActor<APointLight>(Spawn);
	ON_SCOPE_EXIT {
		for (AActor* Actor : {Child, Parent, Path}) if (Actor) World->DestroyActor(Actor);
		TSet<AActor*> WorldActorsAfter;
		for (TActorIterator<AActor> It(World); It; ++It) WorldActorsAfter.Add(*It);
		TestTrue(TEXT("non-target world actor set preserved"), WorldActorsBefore.Num() == WorldActorsAfter.Num()
			&& WorldActorsBefore.Includes(WorldActorsAfter));
		TestEqual(TEXT("owned transient actors preserve world dirty state"), World->GetOutermost()->IsDirty(), bWorldDirtyBefore);
	};
	if (!Child || !Parent || !Path) return false;
	USplineComponent* Spline = NewObject<USplineComponent>(Path, TEXT("NativePickerSpline"), RF_Transient);
	Path->AddInstanceComponent(Spline); Spline->SetupAttachment(Path->GetRootComponent()); Spline->RegisterComponent();
	for (AActor* Actor : {Parent, Path})
		for (UActorComponent* Component : Actor->GetComponents())
			if (const auto* Scene = Cast<USceneComponent>(Component))
				if (!TestFalse(TEXT("fixture selects native no-socket branch"), Scene->HasAnySockets())) return false;
	const FString Name = TEXT("LS_Picker_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = TEXT("/Game/UAI/Transient/T85SequenceNativeTrack/") + Name;
	const FString ObjectPath = PackageName + TEXT(".") + Name;
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	ULevelSequence* Sequence = NewObject<ULevelSequence>(CreatePackage(*PackageName), *Name, RF_Public | RF_Standalone | RF_Transactional);
	Sequence->Initialize(); Sequence->GetMovieScene()->SetPlaybackRange(0, 24000);
	FAssetRegistryModule::AssetCreated(Sequence);
	ON_SCOPE_EXIT {
		if (UObject* Asset = LoadObject<UObject>(nullptr, *ObjectPath))
			TestEqual(TEXT("picker fixture deleted through UE"), ObjectTools::DeleteObjects({Asset}, false), 1);
		UE::UAI::T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, PackageName);
	};
	ISequencerModule& Module = FModuleManager::LoadModuleChecked<ISequencerModule>(TEXT("Sequencer"));
	FModuleManager::Get().LoadModuleChecked<IModuleInterface>(TEXT("MovieSceneTools"));
	auto CreateSequencer = [&](ULevelSequence* Root) {
		FSequencerInitParams Init;
		Init.RootSequence = Root; Init.PlaybackContext = World;
		Init.ViewParams.UniqueName = TEXT("UaiT85NativePicker"); Init.bEditWithinLevelEditor = false;
		return Module.CreateSequencer(Init);
	};
	FGuid ChildGuid, ComponentGuid, ParentGuid, PathGuid;
	auto CapturePersistent = [](const UObject& Object) {
		TMap<FString, FString> Values;
		for (TFieldIterator<FProperty> It(Object.GetClass()); It; ++It) {
			if (It->HasAnyPropertyFlags(CPF_Transient)) continue;
			FString Value; It->ExportText_InContainer(0, Value, &Object, nullptr, const_cast<UObject*>(&Object), PPF_None);
			Values.Add(It->GetName(), Value);
		}
		return Values;
	};
	TMap<FString, FString> AttachBeforePath, AttachSectionBeforePath;
	TArray<TMap<FString, FString>> BeforeSocketCreation;
	const FName SocketName(TEXT("NativePickerSocket"));
	const FName SingleComponentName(TEXT("NativePickerSingle"));
	const FName MultiComponentName(TEXT("NativePickerSecond"));
	const int32 WindowsBefore = FSlateApplication::Get().GetTopLevelWindows().Num();
	{
		const auto Sequencer = CreateSequencer(Sequence);
		ON_SCOPE_EXIT { Sequencer->Close(); };
		ChildGuid = Sequencer->GetHandleToObject(Child, true);
		ComponentGuid = Sequencer->GetHandleToObject(Child->GetLightComponent(), true);
		Sequencer->SetLocalTimeDirectly(FFrameTime(0));
		const auto Host = SNew(SNativeMenuProbeHost);
		const auto Window = SNew(SWindow).ClientSize(FVector2D(800, 800))[Host];
		FSlateApplication::Get().AddWindow(Window, false);
		ON_SCOPE_EXIT { FSlateApplication::Get().DestroyWindowImmediately(Window); };
		auto FindWidget = [](const TSharedRef<SWidget>& Root, FName Type) -> TSharedPtr<SWidget> {
			TArray<TSharedRef<SWidget>> Pending{Root};
			while (!Pending.IsEmpty()) {
				const auto Widget = Pending.Pop(EAllowShrinking::No);
				if (Widget->GetType() == Type) return Widget;
				FChildren* Children = Widget->GetChildren();
				for (int32 Index = 0; Index < Children->Num(); ++Index) Pending.Add(Children->GetChildAt(Index));
			}
			return nullptr;
		};
		auto OpenSubmenu = [&](const TSharedRef<SMultiBoxWidget>& Box, const FString& Label) -> TSharedPtr<SMultiBoxWidget> {
			TArray<TSharedRef<SWidget>> Rows;
			for (const auto& Block : Box->GetMultiBox()->GetBlocks()) {
				const auto Row = Block->MakeWidget(Box, EMultiBlockLocation::None, true, nullptr)->AsWidget();
				if (ReadNativeMenuTexts(Row).Contains(Label)) Rows.Add(Row);
			}
			if (!TestEqual(TEXT("native submenu uniquely selected: ") + Label, Rows.Num(), 1)) return nullptr;
			const auto AnchorWidget = FindWidget(Rows[0], TEXT("SMenuAnchor"));
			if (!AnchorWidget) return nullptr;
			const auto Anchor = StaticCastSharedPtr<SMenuAnchor>(AnchorWidget);
			Host->SetContent(Rows[0]); Window->SlatePrepass(); Anchor->SetIsOpen(true, false);
			const auto Native = Window->GetNativeWindow();
			if (!TestTrue(TEXT("picker submenu stays on hidden owned host"), Anchor->IsOpen()
				&& Anchor->GetMenuWindow() == Window && Native && (!Native->GetOSWindowHandle() || !Native->IsVisible()))) return nullptr;
			return FindNativeMenuBox(Anchor.ToSharedRef());
		};
		auto FindPopup = [&](FName Type) -> TSharedPtr<SWidget> {
			TArray<TSharedRef<SWindow>> Pending = FSlateApplication::Get().GetTopLevelWindows();
			while (!Pending.IsEmpty()) {
				const auto PopupWindow = Pending.Pop(EAllowShrinking::No);
				if (const auto Found = FindWidget(PopupWindow, Type)) {
					const auto Native = PopupWindow->GetNativeWindow();
					if (!TestTrue(TEXT("original native popup has no visible OS window"), Native && (!Native->GetOSWindowHandle() || !Native->IsVisible()))) return nullptr;
					return Found;
				}
				Pending.Append(PopupWindow->GetChildWindows());
			}
			return nullptr;
		};
		UStaticMesh* SocketMesh = nullptr;
		for (int32 Phase = 0; Phase != 4; ++Phase) {
			const bool bExistingSocket = Phase >= 2;
			const FGuid Binding = Phase == 0 ? ChildGuid : Phase == 3 ? PathGuid : ComponentGuid;
			AActor* Target = Phase == 1 ? Path : Parent;
			if (bExistingSocket) {
				if (!SocketMesh) {
					const UStaticMesh* SourceMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
					if (!TestNotNull(TEXT("engine socket fixture source"), SourceMesh)) return false;
					SocketMesh = DuplicateObject<UStaticMesh>(SourceMesh, GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UStaticMesh::StaticClass(), TEXT("NativePickerMesh")));
					SocketMesh->ClearFlags(RF_Public | RF_Standalone); SocketMesh->SetFlags(RF_Transient);
					UStaticMeshSocket* Socket = NewObject<UStaticMeshSocket>(SocketMesh);
					Socket->SocketName = SocketName; SocketMesh->AddSocket(Socket);
				}
				UStaticMeshComponent* MeshComponent = NewObject<UStaticMeshComponent>(Parent, Phase == 2 ? SingleComponentName : MultiComponentName, RF_Transient);
				Parent->AddInstanceComponent(MeshComponent); MeshComponent->SetupAttachment(Parent->GetRootComponent());
				MeshComponent->SetStaticMesh(SocketMesh); MeshComponent->RegisterComponent();
				if (!TestTrue(TEXT("fixture component exposes real mesh socket"), MeshComponent->HasAnySockets() && MeshComponent->DoesSocketExist(SocketName))) return false;
			}
			const FString Label = Phase != 1 ? NSLOCTEXT("F3DAttachTrackEditor", "AddAttach", "Attach").ToString()
				: NSLOCTEXT("FPathTrackEditor", "AddPath", "Path").ToString();
			auto* Storage = Sequencer->GetViewModel()->GetRootSequenceModel()->CastDynamic<FObjectBindingModelStorageExtension>();
			const auto Model = Storage ? Storage->FindModelForObjectBinding(Binding) : nullptr;
			if (!Model) return false;
			const auto RootBox = FindNativeMenuBox(Model->GetAddTrackMenuContent());
			const auto Menu = RootBox ? OpenSubmenu(RootBox.ToSharedRef(), Label) : nullptr;
			if (!Menu) return false;
			const int32 BindingsBefore = Sequence->GetMovieScene()->GetBindings().Num();
			const int32 TracksBefore = Sequence->GetMovieScene()->FindBinding(Binding)->GetTracks().Num();
			if (!TestEqual(TEXT("target track count before picker"), TracksBefore, Phase == 2 ? 1 : 0)) return false;
			if (Phase == 0) {
				const TArray<FString> Options{
					NSLOCTEXT("F3DAttachTrackEditor", "TogglePreserveCurrentTransform", "Preserve Current").ToString(),
					NSLOCTEXT("F3DAttachTrackEditor", "TogglePreserveAllTransform", "Preserve All").ToString(),
					NSLOCTEXT("F3DAttachTrackEditor", "TogglePreserveBake", "Preserve with Bake").ToString(),
					NSLOCTEXT("F3DAttachTrackEditor", "TogglePreserveNone", "None").ToString()};
				for (const FString& Option : Options) {
					TArray<TSharedRef<const FMultiBlock>> Matches;
					for (const auto& Block : Menu->GetMultiBox()->GetBlocks())
						if (ReadNativeMenuTexts(Block->MakeWidget(Menu.ToSharedRef(), EMultiBlockLocation::None, true, nullptr)->AsWidget()).Contains(Option)) Matches.Add(Block);
					if (!TestEqual(TEXT("native attach option unique"), Matches.Num(), 1)) return false;
					if (!TestTrue(TEXT("original option action executes"), Matches[0]->GetDirectActions().Execute())) return false;
					TestTrue(TEXT("option action checks original radio state"), Matches[0]->GetDirectActions().GetCheckState() == ECheckBoxState::Checked);
					TestTrue(TEXT("option action creates no track"), Sequence->GetMovieScene()->FindBinding(Binding)->GetTracks().IsEmpty());
					TestEqual(TEXT("option action creates no binding"), Sequence->GetMovieScene()->GetBindings().Num(), BindingsBefore);
				}
			}
			if (!bExistingSocket) {
			const auto NewMenu = OpenSubmenu(Menu.ToSharedRef(), NSLOCTEXT("FActorPickerTrackEditor", "NewBinding", "New Binding").ToString());
			const auto PickerWidget = NewMenu ? FindWidget(NewMenu.ToSharedRef(), TEXT("SSceneOutliner")) : nullptr;
			if (!TestTrue(TEXT("custom native menu contains original scene outliner"), PickerWidget.IsValid())) return false;
			const auto Picker = StaticCastSharedPtr<SSceneOutliner>(PickerWidget);
			// Populate the original tree through its public Slate lifecycle. Only an
			// item that actually passed native filters can become the selected target.
			Picker->Tick(FGeometry(), FPlatformTime::Seconds(), 0.f);
			const auto Item = Picker->GetTreeItem(FSceneOutlinerTreeItemID(Target));
			if (!TestTrue(TEXT("expected actor is present and interactable in original filtered tree"), Item && Item->CanInteract()
				&& Item->CastTo<FActorTreeItem>() && Item->CastTo<FActorTreeItem>()->Actor.Get() == Target)) return false;
			if (Phase == 0) TestFalse(TEXT("native attach filter excludes bound actor itself"), Picker->GetTreeItem(FSceneOutlinerTreeItemID(Child)).IsValid());
			else TestFalse(TEXT("native path filter excludes actor without spline"), Picker->GetTreeItem(FSceneOutlinerTreeItemID(Parent)).IsValid());
			Picker->SetItemSelection(Item, true, ESelectInfo::Direct);
			TestTrue(TEXT("Direct selection alone creates no track"), Sequence->GetMovieScene()->FindBinding(Binding)->GetTracks().IsEmpty());
			Picker->SetItemSelection(Item, false, ESelectInfo::Direct);
			Picker->SetItemSelection(Item, true, ESelectInfo::OnMouseClick);
			} else {
				const FString TargetLabel = Sequence->GetMovieScene()->FindBinding(ParentGuid)->GetName();
				TArray<TSharedRef<const FMultiBlock>> ExistingMatches;
				for (const auto& Block : Menu->GetMultiBox()->GetBlocks())
					if (Block->GetDirectActions().IsBound() && ReadNativeMenuTexts(Block->MakeWidget(Menu.ToSharedRef(), EMultiBlockLocation::None, true, nullptr)->AsWidget()).Contains(TargetLabel)) ExistingMatches.Add(Block);
				if (!TestEqual(TEXT("original existing target binding action is unique"), ExistingMatches.Num(), 1)) return false;
				if (!TestTrue(TEXT("original existing binding action executes"), ExistingMatches[0]->GetDirectActions().Execute())) return false;
				TestEqual(TEXT("existing binding selection awaits socket before creating track"), Sequence->GetMovieScene()->FindBinding(Binding)->GetTracks().Num(), TracksBefore);
				if (Phase == 3) {
					const auto ComponentPopup = FindPopup(TEXT("SComponentChooserPopup"));
					if (!TestTrue(TEXT("original multi-component chooser exists"), ComponentPopup.IsValid())) return false;
					// SNew records the full stringified template type as widget type.
					const auto ListWidget = FindWidget(ComponentPopup.ToSharedRef(), TEXT("SListView< TSharedPtr<FName> >"));
					if (!TestTrue(TEXT("original multi-component chooser list exists"), ListWidget.IsValid())) return false;
					// Consume the public list widget inside the original chooser. No
					// dependency on its private class or replacement of its delegate.
					const auto ComponentList = StaticCastSharedPtr<SListView<TSharedPtr<FName>>>(ListWidget);
					TSharedPtr<FName> SelectedComponent;
					int32 Matches = 0;
					for (const auto& Item : ComponentList->GetItems())
						if (Item && *Item == MultiComponentName) { SelectedComponent = Item; ++Matches; }
					if (!TestTrue(TEXT("native component list contains both socket components and unique expected second"), ComponentList->GetItems().Num() == 2 && Matches == 1)) return false;
					ComponentList->SetSelection(SelectedComponent, ESelectInfo::OnMouseClick);
					TestEqual(TEXT("component selection alone still awaits socket"), Sequence->GetMovieScene()->FindBinding(Binding)->GetTracks().Num(), TracksBefore);
				} else TestFalse(TEXT("one socket component does not require component chooser"), FindPopup(TEXT("SComponentChooserPopup")).IsValid());
				const auto SocketWidget = FindPopup(TEXT("SSocketChooserPopup"));
				if (!TestTrue(TEXT("original socket chooser exists"), SocketWidget.IsValid())) return false;
				const auto SocketPopup = StaticCastSharedPtr<SSocketChooserPopup>(SocketWidget);
				if (!TestTrue(TEXT("original socket chooser represents exact selected component"), SocketPopup->SceneComponent.IsValid()
					&& SocketPopup->SceneComponent->GetFName() == (Phase == 2 ? SingleComponentName : MultiComponentName))) return false;
				TSharedPtr<SSocketChooserPopup::FSocketInfo> SelectedSocket;
				int32 SocketMatches = 0;
				for (const auto& Item : SocketPopup->SocketListView->GetItems())
					if (Item && Item->Description.Name == SocketName && Item->Description.Type == EComponentSocketType::Socket) { SelectedSocket = Item; ++SocketMatches; }
				if (!TestEqual(TEXT("expected socket is unique in original native list"), SocketMatches, 1)) return false;
				SocketPopup->SocketListView->SetSelection(SelectedSocket, ESelectInfo::Direct);
				TestEqual(TEXT("Direct socket selection does not create"), Sequence->GetMovieScene()->FindBinding(Binding)->GetTracks().Num(), TracksBefore);
				SocketPopup->SocketListView->ClearSelection();
				SocketPopup->SocketListView->SetSelection(SelectedSocket, ESelectInfo::OnMouseClick);
			}
			const auto* After = Sequence->GetMovieScene()->FindBinding(Binding);
			if (!TestTrue(TEXT("same native picker delegate creates one track"), After && After->GetTracks().Num() == TracksBefore + 1)) return false;
			TestEqual(TEXT("picker creates binding only for new-binding branch"), Sequence->GetMovieScene()->GetBindings().Num(), BindingsBefore + (bExistingSocket ? 0 : 1));
			const FGuid TargetGuid = Sequencer->GetHandleToObject(Target, false);
			if (Phase == 0) ParentGuid = TargetGuid; else if (Phase == 1) PathGuid = TargetGuid;
			TestTrue(TEXT("native target binding resolves exact selected actor"), HasExactBoundObject(*Sequencer, TargetGuid, Target));
			TestTrue(TEXT("target actor gets no own tracks"), Sequence->GetMovieScene()->FindBinding(TargetGuid)->GetTracks().IsEmpty());
			const auto* AttachTrack = Sequence->GetMovieScene()->FindBinding(ChildGuid)->GetTracks()[0];
			if (Phase == 0) {
				AttachBeforePath = CapturePersistent(*AttachTrack);
				AttachSectionBeforePath = CapturePersistent(*AttachTrack->GetAllSections()[0]);
			} else {
				TestTrue(TEXT("Path creation preserves complete prior Attach track and section"),
					AttachBeforePath.OrderIndependentCompareEqual(CapturePersistent(*AttachTrack))
					&& AttachSectionBeforePath.OrderIndependentCompareEqual(CapturePersistent(*AttachTrack->GetAllSections()[0])));
			}
			if (Phase == 1) {
				const auto* PathTrack = Sequence->GetMovieScene()->FindBinding(ComponentGuid)->GetTracks()[0];
				BeforeSocketCreation = {CapturePersistent(*PathTrack), CapturePersistent(*PathTrack->GetAllSections()[0])};
			} else if (bExistingSocket) {
				const auto* PathTrack = Sequence->GetMovieScene()->FindBinding(ComponentGuid)->GetTracks()[0];
				TestTrue(TEXT("socket creation preserves complete prior Path track and channel"), BeforeSocketCreation[0].OrderIndependentCompareEqual(CapturePersistent(*PathTrack))
					&& BeforeSocketCreation[1].OrderIndependentCompareEqual(CapturePersistent(*PathTrack->GetAllSections()[0])));
			}
			AddInfo(FString::Printf(TEXT("NativeActorPicker phase=%d menu=%s same_delegate_create=1 new_binding=%d socket=%d multi_component=%d"), Phase, *Label, !bExistingSocket, bExistingSocket, Phase == 3));
		}
	}
	auto Check = [&](ISequencer& Sequencer) {
		for (int32 Phase = 0; Phase != 4; ++Phase) {
			const auto* Binding = Sequence->GetMovieScene()->FindBinding(Phase == 0 ? ChildGuid : Phase == 3 ? PathGuid : ComponentGuid);
			if (!TestTrue(TEXT("fixed expected binding track count"), Binding && Binding->GetTracks().Num() == (Phase == 1 || Phase == 2 ? 2 : 1))) return false;
			const auto* Track = Binding->GetTracks()[Phase == 2 ? 1 : 0];
			if (!TestTrue(TEXT("fixed expected track class"), Track->GetClass() == (Phase != 1 ? UMovieScene3DAttachTrack::StaticClass() : UMovieScene3DPathTrack::StaticClass()))
				|| !TestEqual(TEXT("fixed one constraint section"), Track->GetAllSections().Num(), 1)) return false;
			const auto* Section = Cast<UMovieScene3DConstraintSection>(Track->GetAllSections()[0]);
			if (!TestTrue(TEXT("fixed native constraint range"), Section && Section->GetRange() == TRange<FFrameNumber>(0, 24000))) return false;
			TestEqual(TEXT("constraint stores selected actor binding"), Section->GetConstraintBindingID().GetGuid(), Phase == 1 ? PathGuid : ParentGuid);
			if (Phase != 1) {
				const auto* Attach = Cast<UMovieScene3DAttachSection>(Section);
				TestTrue(TEXT("native selected component and socket stored exactly"), Attach && Attach->AttachSocketName == (Phase == 0 ? NAME_None : SocketName)
					&& Attach->AttachComponentName == (Phase == 0 ? NAME_None : Phase == 2 ? SingleComponentName : MultiComponentName));
			}
		}
		TestEqual(TEXT("existing binding selection does not duplicate bindings"), Sequence->GetMovieScene()->GetBindings().Num(), 4);
		TestTrue(TEXT("target parent has no tracks"), Sequence->GetMovieScene()->FindBinding(ParentGuid)->GetTracks().IsEmpty());
		TestTrue(TEXT("original bindings resolve after native creation"), HasExactBoundObject(Sequencer, ChildGuid, Child)
			&& HasExactBoundObject(Sequencer, ComponentGuid, Child->GetLightComponent())
			&& HasExactBoundObject(Sequencer, ParentGuid, Parent) && HasExactBoundObject(Sequencer, PathGuid, Path));
		TestTrue(TEXT("no root track created"), Sequence->GetMovieScene()->GetTracks().IsEmpty());
		return !HasAnyErrors();
	};
	{ const auto Sequencer = CreateSequencer(Sequence); ON_SCOPE_EXIT { Sequencer->Close(); }; if (!Check(*Sequencer)) return false; }
	TArray<TMap<FString, FString>> PersistentBefore;
	for (const FGuid Guid : {ChildGuid, ComponentGuid, PathGuid})
	for (const auto* Track : Sequence->GetMovieScene()->FindBinding(Guid)->GetTracks()) {
		PersistentBefore.Add(CapturePersistent(*Track));
		PersistentBefore.Add(CapturePersistent(*Track->GetAllSections()[0]));
	}
	FSavePackageArgs SaveArgs; SaveArgs.TopLevelFlags = RF_Public | RF_Standalone; SaveArgs.SaveFlags = SAVE_NoError;
	if (!UPackage::SavePackage(Sequence->GetOutermost(), Sequence, *Filename, SaveArgs)) return false;
	TWeakObjectPtr<ULevelSequence> OldSequence = Sequence; Sequence = nullptr;
	FString Error;
	if (!UE::UAI::T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)) { AddError(Error); return false; }
	TestFalse(TEXT("picker sequence old object independently exits"), OldSequence.IsValid(true, true));
	Sequence = LoadObject<ULevelSequence>(nullptr, *ObjectPath);
	if (!Sequence) return false;
	{ const auto Sequencer = CreateSequencer(Sequence); ON_SCOPE_EXIT { Sequencer->Close(); }; if (!Check(*Sequencer)) return false; }
	int32 PersistentIndex = 0;
	for (const FGuid Guid : {ChildGuid, ComponentGuid, PathGuid})
	for (const auto* Track : Sequence->GetMovieScene()->FindBinding(Guid)->GetTracks()) {
		TestTrue(TEXT("complete track persistence"), PersistentBefore[PersistentIndex++].OrderIndependentCompareEqual(CapturePersistent(*Track)));
		TestTrue(TEXT("complete section and channel persistence"), PersistentBefore[PersistentIndex++].OrderIndependentCompareEqual(CapturePersistent(*Track->GetAllSections()[0])));
	}
	TestFalse(TEXT("picker disk readback is clean"), Sequence->GetOutermost()->IsDirty());
	TestEqual(TEXT("picker hosts are removed"), FSlateApplication::Get().GetTopLevelWindows().Num(), WindowsBefore);
	AddInfo(TEXT("NativeActorPicker attach_actor=1 path_component=1 options_no_create=4 existing_binding=2 single_socket_component=1 multi_socket_component=1 saved_unloaded_reloaded=1 exact_target_bindings=1"));
	return !HasAnyErrors();
}

#include "Core/AssetPreview/UeAgentAssetPreviewState.h"
#include "Core/AssetPreview/UeAgentAssetEditorLifetime.h"
#include "Core/AssetPreview/UeAgentAssetPreviewScheduling.h"

#include "Editor.h"
#include "Core/EditorCapture/UeAgentEditorWidgetTree.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/Commands/InputBindingManager.h"
#include "Framework/Commands/UICommandList.h"
#include "ISequencer.h"
#include "ISequencerModule.h"
#include "Modules/ModuleManager.h"
#include "MovieSceneSequence.h"
#include "NiagaraSystem.h"
#include "NiagaraEmitter.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/Docking/SDockTab.h"

namespace UE::UAI::AssetPreview
{
	namespace
	{
		struct FSequencerBinding
		{
			TWeakObjectPtr<UObject> Asset;
			TWeakPtr<FTabManager> Tabs;
			TWeakPtr<ISequencer> Sequencer;
		};
		TArray<FSequencerBinding> Sequencers;
		TArray<TWeakPtr<ISequencer>> ObservedSequencers;
		FDelegateHandle CreatedHandle;
		FDelegateHandle ModuleHandle;

		void ObserveModule()
		{
			if (CreatedHandle.IsValid()) return;
			if (auto* Module = FModuleManager::GetModulePtr<ISequencerModule>(TEXT("Sequencer")))
				CreatedHandle = Module->RegisterOnSequencerCreated(FOnSequencerCreated::FDelegate::CreateLambda(
					[](TSharedRef<ISequencer> Sequencer) { ObservedSequencers.Add(Sequencer); }));
		}
	}

	void StartSequencerObservation()
	{
		if (ModuleHandle.IsValid()) return;
		ModuleHandle = FModuleManager::Get().OnModulesChanged().AddLambda(
			[](FName Name, EModuleChangeReason Reason)
			{
				if (Name != TEXT("Sequencer")) return;
				if (Reason == EModuleChangeReason::ModuleLoaded) ObserveModule();
				else if (Reason == EModuleChangeReason::ModuleUnloaded)
				{ CreatedHandle.Reset(); ObservedSequencers.Reset(); Sequencers.Reset(); }
			});
		ObserveModule();
	}

	void StopSequencerObservation()
	{
		CancelDeferredPreviewWork();
		FModuleManager::Get().OnModulesChanged().Remove(ModuleHandle);
		ModuleHandle.Reset();
		if (auto* Module = FModuleManager::GetModulePtr<ISequencerModule>(TEXT("Sequencer")))
			if (CreatedHandle.IsValid()) Module->UnregisterOnSequencerCreated(CreatedHandle);
		CreatedHandle.Reset();
		ObservedSequencers.Reset();
		Sequencers.Reset();
	}

	TSharedPtr<ISequencer> FindPreviewSequencer(UObject* Asset)
	{
		ObservedSequencers.RemoveAll([](const TWeakPtr<ISequencer>& Item) { return !Item.IsValid(); });
		TSharedPtr<ISequencer> Exact;
		for (const auto& Item : ObservedSequencers)
			if (const auto Candidate = Item.Pin())
				if (Candidate->GetRootMovieSceneSequence() == Asset)
				{
					if (Exact && Exact != Candidate) return nullptr;
					Exact = Candidate;
				}
		if (Exact) return Exact;
		auto* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
		auto* Editor = Subsystem && Asset ? Subsystem->FindEditorForAsset(Asset, false) : nullptr;
		Sequencers.RemoveAll([](const FSequencerBinding& Item)
			{ return !Item.Asset.IsValid() || !Item.Tabs.IsValid() || !Item.Sequencer.IsValid(); });
		if (!Editor) return nullptr;
		for (const auto& Item : Sequencers)
			if (Item.Asset == Asset && Item.Tabs.Pin() == Editor->GetAssociatedTabManager()) return Item.Sequencer.Pin();
		ObservedSequencers.RemoveAll([](const TWeakPtr<ISequencer>& Item) { return !Item.IsValid(); });
		const auto Tabs = Editor->GetAssociatedTabManager();
		const auto Tab = Tabs ? Tabs->FindExistingLiveTab(FTabId(TEXT("NiagaraSystemEditor_Sequencer"))) : nullptr;
		if (Tab)
			for (const auto& Item : ObservedSequencers)
				if (const auto Sequencer = Item.Pin())
					if (EditorCapture::WidgetTree::Contains(Tab->GetContent(), Sequencer->GetSequencerWidget()))
					{ Sequencers.Add({Asset, Tabs, Sequencer}); return Sequencer; }
		return nullptr;
	}

	bool OpenAssetEditor(UObject*& Asset, const bool bFocusExistingEditor)
	{
		auto* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
		if (!Subsystem || !Asset) return false;
		if (!bFocusExistingEditor && Subsystem->FindEditorForAsset(Asset, false)) return true;
		auto Ownership = BeginAssetEditorOpen(Asset);
		const FString AssetPath = Asset->GetPathName();
		const TWeakObjectPtr<UObject> BeforeOpen = Asset;
		auto OpenNative = [&]()
		{
			const bool bOpened = Subsystem->OpenEditorForAsset(Asset);
			// The native subsystem only repairs its own local pointer. Use the
			// pre-open weak identity without dereferencing the possibly freed raw one.
			Asset = BeforeOpen.Get();
			if (!Asset) Asset = FindObject<UObject>(nullptr, *AssetPath);
			Ownership->Complete(Asset);
			return bOpened && IsValid(Asset);
		};
		if ((!Asset->IsA<UNiagaraSystem>() && !Asset->IsA<UNiagaraEmitter>()) || Subsystem->FindEditorForAsset(Asset, false))
			return OpenNative();
		// Native Niagara sequences have no playback context identifying the system.
		// Associate only a unique sequence created synchronously by this exact open.
		auto& Module = FModuleManager::LoadModuleChecked<ISequencerModule>(TEXT("Sequencer"));
		TArray<TWeakPtr<ISequencer>> Created;
		const auto Handle = Module.RegisterOnSequencerCreated(FOnSequencerCreated::FDelegate::CreateLambda(
			[&Created](TSharedRef<ISequencer> Sequencer)
			{
				auto* Root = Sequencer->GetRootMovieSceneSequence();
				if (Root && Root->GetClass()->GetFName() == TEXT("NiagaraSequence")) Created.Add(Sequencer);
			}));
		const bool bOpened = OpenNative();
		Module.UnregisterOnSequencerCreated(Handle);
		if (bOpened && Created.Num() == 1)
			if (auto* Editor = Subsystem->FindEditorForAsset(Asset, false))
				Sequencers.Add({Asset, Editor->GetAssociatedTabManager(), Created[0]});
		return bOpened;
	}

	bool ResetNiagaraPreview(UObject* Asset, UE::UAI::Contract::FDiagnostic& OutError)
	{
		auto* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
		auto* Editor = Subsystem && Asset && (Asset->IsA<UNiagaraSystem>() || Asset->IsA<UNiagaraEmitter>())
			? Subsystem->FindEditorForAsset(Asset, false) : nullptr;
		if (!Editor || Editor->GetEditorName() != TEXT("Niagara"))
		{ OutError = PreviewFailure(TEXT("niagara_preview_toolkit_unavailable"), !Subsystem ? TEXT("The asset editor subsystem is unavailable for Niagara preview.") : !Asset ? TEXT("The Niagara preview asset is unavailable.") : !Editor ? TEXT("The Niagara asset has no open preview editor.") : TEXT("The asset's open editor is not a Niagara toolkit."), Asset); return false; }
		const auto Command = FInputBindingManager::Get().FindCommandInContext(TEXT("NiagaraEditor"), TEXT("ResetSimulation"));
		const auto Commands = static_cast<FAssetEditorToolkit*>(Editor)->GetToolkitCommands();
		if (!Command || !Commands->TryExecuteAction(Command.ToSharedRef()))
		{ OutError = PreviewFailure(TEXT("niagara_preview_reset_unavailable"), !Command ? TEXT("The Niagara ResetSimulation command is not registered.") : TEXT("The Niagara editor rejected the ResetSimulation command."), Asset, TEXT("ResetSimulation")); return false; }
		return true;
	}
}

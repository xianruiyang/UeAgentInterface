#include "Core/AssetPreview/UeAgentAssetEditorLifetime.h"

#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "CoreGlobals.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Framework/Docking/TabManager.h"
#include "HAL/ThreadSafeCounter.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/PackageReload.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Curves/CurveFloat.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "UObject/StrongObjectPtr.h"
#endif

namespace UE::UAI::AssetPreview
{
	namespace
	{
		struct FWindow
		{
			IAssetEditorInstance* Editor = nullptr;
			TWeakPtr<FTabManager> Tabs;
			bool bHasTabs = false;
			// Identity only. Never dereference an entry after a reload/GC.
			TSet<UObject*> Assets;
			TSet<UObject*> RegisteredAssets;
		};

		struct FResource
		{
			FString Path;
			UObject* Identity = nullptr;
			TWeakObjectPtr<UObject> Asset;
			TArray<FWindow> Windows;
			FThreadSafeCounter Users;
			int32 Openers = 0;
			bool bAwaitingReopen = false;
			bool bOwnedBeforeReload = false;
			bool bWasManaged = false;

			void RefreshPath()
			{
				if (auto* Current = Asset.Get()) Path = Current->GetPathName();
			}
		};

		bool IsCurrent(const FWindow& Window, const TArray<IAssetEditorInstance*>& Current)
		{
			if (!Current.Contains(Window.Editor)) return false;
			const auto Tabs = Window.Editor->GetAssociatedTabManager();
			return Window.bHasTabs ? Window.Tabs.IsValid() && Tabs == Window.Tabs.Pin() : !Tabs;
		}

		class FTracker : public TSharedFromThis<FTracker, ESPMode::ThreadSafe>
		{
		public:
			TArray<TSharedPtr<FResource, ESPMode::ThreadSafe>> Resources;
			TWeakObjectPtr<UAssetEditorSubsystem> Subsystem;
			FDelegateHandle OpenedHandle;
			FDelegateHandle ClosedHandle;
			FDelegateHandle ReloadHandle;
			FTSTicker::FDelegateHandle SettleHandle;
			TArray<FWindow> BeforeReload;
			bool bReloading = false;

			void Bind()
			{
				check(IsInGameThread());
				if (!ReloadHandle.IsValid())
					ReloadHandle = FCoreUObjectDelegates::OnPackageReloaded.AddSP(AsShared(), &FTracker::Reload);
				if (!Subsystem.IsValid() && GEditor)
				{
					OpenedHandle.Reset();
					ClosedHandle.Reset();
					Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
				}
				if (auto* Editors = Subsystem.Get())
				{
					if (!OpenedHandle.IsValid())
						OpenedHandle = Editors->OnAssetOpenedInEditor().AddSP(AsShared(), &FTracker::Opened);
					if (!ClosedHandle.IsValid())
						ClosedHandle = Editors->OnAssetClosedInEditor().AddSP(AsShared(), &FTracker::Closed);
				}
			}

			void PruneEmptyResources()
			{
				check(IsInGameThread());
				if (bReloading) return;
				Resources.RemoveAll([](const auto& Resource)
				{
					return Resource->Users.GetValue() == 0 && Resource->Openers == 0
						&& Resource->Windows.IsEmpty() && !Resource->bAwaitingReopen;
				});
			}

			void RequestPrune()
			{
				if (IsInGameThread()) PruneEmptyResources();
				else
				{
					// Lease destruction can happen on a worker. Registry arrays
					// remain GT-owned; no UObject work is needed for this sweep.
					const TWeakPtr<FTracker, ESPMode::ThreadSafe> WeakOwner = AsShared();
					AsyncTask(ENamedThreads::GameThread, [WeakOwner]()
					{
						if (const auto Owner = WeakOwner.Pin()) Owner->PruneEmptyResources();
					});
				}
			}

			FWindow ReadWindow(IAssetEditorInstance* Editor) const
			{
				FWindow Window;
				Window.Editor = Editor;
				Window.Tabs = Editor->GetAssociatedTabManager();
				Window.bHasTabs = Window.Tabs.IsValid();
				if (auto* Editors = Subsystem.Get())
					for (auto* Asset : Editors->GetAllEditedAssets())
						if (Asset && Editors->FindEditorsForAsset(Asset).Contains(Editor))
						{
							Window.RegisteredAssets.Add(Asset);
							if (Asset->IsAsset()) Window.Assets.Add(Asset);
						}
				return Window;
			}

			TSharedPtr<FResource, ESPMode::ThreadSafe> FindOrAdd(UObject* Asset)
			{
				for (const auto& Resource : Resources)
					if (Resource->Identity == Asset && Resource->Asset.Get() == Asset) return Resource;
				auto Resource = MakeShared<FResource, ESPMode::ThreadSafe>();
				Resource->Path = Asset->GetPathName();
				Resource->Identity = Asset;
				Resource->Asset = Asset;
				Resources.Add(Resource);
				return Resource;
			}

			TArray<FWindow> Snapshot() const
			{
				TArray<FWindow> Result;
				if (auto* Editors = Subsystem.Get())
					for (auto* Editor : Editors->GetAllOpenEditors())
					{
						Result.Add(ReadWindow(Editor));
					}
				return Result;
			}

			bool WasPresent(IAssetEditorInstance* Editor, const TArray<FWindow>& Before) const
			{
				const TArray<IAssetEditorInstance*> Current{Editor};
				for (const auto& Window : Before)
					if (Window.Editor == Editor && IsCurrent(Window, Current)) return true;
				return false;
			}

			void Track(const TSharedPtr<FResource, ESPMode::ThreadSafe>& Resource, FWindow Window)
			{
				Resource->Windows.RemoveAll([&](const FWindow& Existing) { return Existing.Editor == Window.Editor; });
				Resource->Windows.Add(MoveTemp(Window));
				Resource->bWasManaged = true;
			}

			void Track(const TSharedPtr<FResource, ESPMode::ThreadSafe>& Resource, IAssetEditorInstance* Editor)
			{
				auto* Editors = Subsystem.Get();
				if (!Editors || !Editors->GetAllOpenEditors().Contains(Editor)) return;
				// Only a proven open/replacement (or explicit adoption) reaches
				// Track. Its final snapshot includes all initial registrations,
				// even when the first opened event preceded further assets.
				Track(Resource, ReadWindow(Editor));
			}

			void Opened(UObject* Asset, IAssetEditorInstance* Editor)
			{
				for (const auto& Resource : Resources)
				{
					// Later registrations are observed, but do not expand the
					// asset set that this scope is authorized to close.
					for (auto& Window : Resource->Windows)
						if (Window.Editor == Editor) Window.RegisteredAssets.Add(Asset);
					// Only a proven native replacement inherits ownership.
					if (bReloading && Resource->bAwaitingReopen && Resource->Identity == Asset
						&& !WasPresent(Editor, BeforeReload))
						Track(Resource, Editor);
				}
				for (auto& Window : BeforeReload)
					if (Window.Editor == Editor) Window.RegisteredAssets.Add(Asset);
			}

			void Closed(UObject* Asset, IAssetEditorInstance* Editor)
			{
				// Native broadcasts before removing each (asset, editor) pair.
				// Keep a multi-asset toolkit until its last registration closes,
				// including transient preview objects. Never infer a new user's
				// ownership from a subsequently reused raw toolkit address.
				auto RemoveRegistration = [&](TArray<FWindow>& Windows)
				{
					for (auto& Window : Windows)
						if (Window.Editor == Editor)
						{
							Window.RegisteredAssets.Remove(Asset);
							Window.Assets.Remove(Asset);
						}
					Windows.RemoveAll([&](const FWindow& Window)
						{ return Window.Editor == Editor && Window.RegisteredAssets.IsEmpty(); });
				};
				for (const auto& Resource : Resources) RemoveRegistration(Resource->Windows);
				RemoveRegistration(BeforeReload);
				PruneEmptyResources();
			}

			void Reload(EPackageReloadPhase Phase, FPackageReloadedEvent* Event)
			{
				if (Phase == EPackageReloadPhase::PreBatch)
				{
					// ReloadPackages can run several batches synchronously,
					// before the last batch's deferred ticker gets a turn.
					if (SettleHandle.IsValid())
					{
						FTSTicker::GetCoreTicker().RemoveTicker(SettleHandle);
						SettleReload(0);
					}
					bReloading = true;
					BeforeReload = Snapshot();
					if (auto* Editors = Subsystem.Get())
					{
						const auto Current = Editors->GetAllOpenEditors();
						for (const auto& Resource : Resources)
						{
							Resource->Windows.RemoveAll([&](const FWindow& Window) { return !Current.Contains(Window.Editor); });
							Resource->bOwnedBeforeReload = !Resource->Windows.IsEmpty();
						}
					}
				}
				else if (Phase == EPackageReloadPhase::PrePackageFixup && Event)
				{
					for (const auto& Resource : Resources)
					{
						UObject* Replacement = nullptr;
						if (Event->GetRepointedObject(Resource->Identity, Replacement))
						{
							// Native close events may run before this delegate's
							// fixup callback. PreBatch preserves the prior scope.
							Resource->bAwaitingReopen |= Resource->bOwnedBeforeReload;
							Resource->Identity = Replacement;
							Resource->Asset = Replacement;
							if (Replacement) Resource->Path = Replacement->GetPathName();
						}
						for (auto& Window : Resource->Windows)
							for (const auto& Pair : Event->GetRepointedObjects())
								if (Window.Assets.Remove(Pair.Key) && Pair.Value) Window.Assets.Add(Pair.Value);
					}
				}
				else if (Phase == EPackageReloadPhase::PostBatchPreGC)
				{
					// Native close callbacks have returned and reopening has not
					// started. Observe removals now, before allocator reuse can
					// make a new toolkit appear to be an old pointer.
					if (auto* Editors = Subsystem.Get())
					{
						const auto Current = Editors->GetAllOpenEditors();
						BeforeReload.RemoveAll([&](const FWindow& Window) { return !Current.Contains(Window.Editor); });
						for (const auto& Resource : Resources)
							Resource->Windows.RemoveAll([&](const FWindow& Window) { return !Current.Contains(Window.Editor); });
					}
				}
				else if (Phase == EPackageReloadPhase::PostBatchPostGC)
				{
					// Multicast order is not an ownership guarantee. Settle only
					// after every subsystem has had a chance to reopen its editors.
					if (!SettleHandle.IsValid())
						SettleHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateSP(
							AsShared(), &FTracker::SettleReload));
				}
			}

			bool SettleReload(float)
			{
				SettleHandle.Reset();
				if (auto* Editors = Subsystem.Get())
				{
					const auto Current = Editors->GetAllOpenEditors();
					for (const auto& Resource : Resources)
					{
						if (Resource->bAwaitingReopen)
							if (auto* Asset = Resource->Asset.Get())
								for (auto* Editor : Editors->FindEditorsForAsset(Asset))
									if (!WasPresent(Editor, BeforeReload)) Track(Resource, Editor);
						Resource->Windows.RemoveAll([&](const FWindow& Window) { return !Current.Contains(Window.Editor); });
						Resource->bAwaitingReopen = false;
						Resource->bOwnedBeforeReload = false;
					}
				}
				BeforeReload.Reset();
				bReloading = false;
				PruneEmptyResources();
				return false;
			}

			void Detach()
			{
				if (auto* Editors = Subsystem.Get())
				{
					Editors->OnAssetOpenedInEditor().Remove(OpenedHandle);
					Editors->OnAssetClosedInEditor().Remove(ClosedHandle);
				}
				OpenedHandle.Reset();
				ClosedHandle.Reset();
				Subsystem.Reset();
				FCoreUObjectDelegates::OnPackageReloaded.Remove(ReloadHandle);
				ReloadHandle.Reset();
				if (SettleHandle.IsValid()) FTSTicker::GetCoreTicker().RemoveTicker(SettleHandle);
				SettleHandle.Reset();
				BeforeReload.Reset();
				bReloading = false;
			}

			~FTracker()
			{
				FCoreUObjectDelegates::OnPackageReloaded.Remove(ReloadHandle);
				if (SettleHandle.IsValid()) FTSTicker::GetCoreTicker().RemoveTicker(SettleHandle);
				// Subsystem delegates are weak AddSP bindings. Do not query a
				// UObject here: module destruction can follow UObject shutdown.
			}
		};

		TSharedPtr<FTracker, ESPMode::ThreadSafe> Tracker;

		TSharedRef<FTracker, ESPMode::ThreadSafe> GetTracker()
		{
			if (!Tracker) Tracker = MakeShared<FTracker, ESPMode::ThreadSafe>();
			Tracker->Bind();
			return Tracker.ToSharedRef();
		}

		class FOpenScope final : public IAssetEditorOpenScope
		{
		public:
			FOpenScope(UObject* Asset, TSharedRef<FTracker, ESPMode::ThreadSafe> InOwner)
				: Owner(MoveTemp(InOwner)), Resource(Owner->FindOrAdd(Asset)), Before(Owner->Snapshot())
			{
				++Resource->Openers;
				// A native toolkit may unregister its old document before adding
				// the requested one. Preserve proven ownership across that gap,
				// independently of the live registration-driven registry.
				for (const auto& Window : Before)
				{
					FWindow Managed = Window;
					Managed.Assets.Reset();
					bool bManaged = false;
					const TArray<IAssetEditorInstance*> Current{Window.Editor};
					for (const auto& ExistingResource : Owner->Resources)
						for (const auto& Existing : ExistingResource->Windows)
							if (Existing.Editor == Window.Editor && IsCurrent(Existing, Current))
							{
								Managed.Assets.Append(Existing.Assets);
								bManaged = true;
							}
					if (bManaged) ManagedBefore.Add(MoveTemp(Managed));
				}
			}
			~FOpenScope() override
			{
				--Resource->Openers;
				Owner->PruneEmptyResources();
			}
			void Complete(UObject* Asset) override
			{
				if (!Asset) return;
				Resource->Identity = Asset;
				Resource->Asset = Asset;
				Resource->Path = Asset->GetPathName();
				if (auto* Editors = Owner->Subsystem.Get())
					for (auto* Editor : Editors->FindEditorsForAsset(Asset))
					{
						if (!Owner->WasPresent(Editor, Before))
						{
							Owner->Track(Resource, Editor);
							continue;
						}
						const TArray<IAssetEditorInstance*> Current{Editor};
						const auto* Managed = ManagedBefore.FindByPredicate([&](const FWindow& Window)
							{ return Window.Editor == Editor && IsCurrent(Window, Current); });
						if (!Managed) continue;
						const auto* Previous = Before.FindByPredicate([&](const FWindow& Window)
							{ return Window.Editor == Editor && IsCurrent(Window, Current); });
						if (!Previous) continue;
						auto Continued = Owner->ReadWindow(Editor);
						const auto CurrentAssets = MoveTemp(Continued.Assets);
						Continued.Assets = Managed->Assets;
						// Only registrations introduced by this synchronous UAI open
						// extend the scope. Pre-existing user additions stay excluded.
						for (auto* CurrentAsset : CurrentAssets)
							if (!Previous->Assets.Contains(CurrentAsset)) Continued.Assets.Add(CurrentAsset);
						for (const auto& ExistingResource : Owner->Resources)
							for (auto& Window : ExistingResource->Windows)
								if (Window.Editor == Editor && IsCurrent(Window, Current))
								{
									Window.Assets.Append(Continued.Assets);
									Window.RegisteredAssets = Continued.RegisteredAssets;
								}
						Owner->Track(Resource, MoveTemp(Continued));
					}
			}
		private:
			TSharedRef<FTracker, ESPMode::ThreadSafe> Owner;
			TSharedPtr<FResource, ESPMode::ThreadSafe> Resource;
			TArray<FWindow> Before;
			TArray<FWindow> ManagedBefore;
		};

		class FLease final : public IAssetEditorLease
		{
		public:
			FLease(TSharedRef<FTracker, ESPMode::ThreadSafe> InOwner, TSharedPtr<FResource, ESPMode::ThreadSafe> InResource)
				: Owner(MoveTemp(InOwner)), Resource(MoveTemp(InResource)) { Resource->Users.Increment(); }

			~FLease() override
			{
				// No implicit close from destruction. The registry retains any
				// remaining UAI windows for a later public session to acquire.
				if (!bComplete)
				{
					Resource->Users.Decrement();
					Owner->RequestPrune();
				}
			}

			TSharedPtr<FJsonObject> Describe() const override
			{
				check(IsInGameThread());
				Resource->RefreshPath();
				auto Details = MakeShared<FJsonObject>();
				Details->SetStringField(TEXT("asset_path"), Resource->Path);
				Details->SetStringField(TEXT("ownership"), Resource->bWasManaged ? TEXT("managed") : TEXT("borrowed"));
				Details->SetNumberField(TEXT("managed_windows"), Resource->Windows.Num());
				Details->SetNumberField(TEXT("holders"), Resource->Users.GetValue());
				Details->SetBoolField(TEXT("cleanup_complete"), bComplete);
				return Details;
			}

			bool Close(bool bRequestClose, UE::UAI::Contract::FDiagnostic& OutDiagnostic,
				TSharedPtr<FJsonObject>& OutDetails) override
			{
				check(IsInGameThread());
				Resource->RefreshPath();
				bTryClosePending |= bRequestClose;
				OutDiagnostic = {};
				auto Pending = [&](const TCHAR* Code, const TCHAR* Message, const FString& Detail = FString())
				{
					OutDiagnostic = UE::UAI::Contract::FDiagnostic::Failure(Code, Message, TEXT("asset_editor_cleanup"));
					OutDiagnostic.SubjectId = Resource->Path;
					OutDiagnostic.TargetPath = Resource->Path;
					OutDiagnostic.Source.JsonPath = TEXT("/asset_editor/close");
					if (!Detail.IsEmpty()) OutDiagnostic.ActualValue = MakeShared<FJsonValueString>(Detail);
					OutDiagnostic.ResourceDimension = TEXT("managed_asset_editor_windows");
					OutDiagnostic.ResourceObserved = Resource->Windows.Num();
					OutDiagnostic.RecoveryAction = TEXT("Recover the existing operation and close again after resolving the reported editor state.");
					OutDetails = Describe();
					return false;
				};
				auto Complete = [&]()
				{
					if (!bComplete) { Resource->Users.Decrement(); bComplete = true; }
					Owner->PruneEmptyResources();
					OutDetails = Describe();
					return true;
				};
				if (bComplete) return Complete();
				if (Resource->Users.GetValue() > 1) return Complete();
				if (Owner->bReloading || Resource->bAwaitingReopen)
					return Pending(TEXT("asset_editor_reload_pending"), TEXT("Asset editor replacement is still settling; cleanup retains ownership."));
				auto* Editors = Owner->Subsystem.Get();
				if (!Editors)
					return Pending(TEXT("asset_editor_cleanup_unavailable"), TEXT("The asset editor subsystem is unavailable; window cleanup is unconfirmed."));
				auto Current = Editors->GetAllOpenEditors();
				Resource->Windows.RemoveAll([&](const FWindow& Window) { return !Current.Contains(Window.Editor); });
				if (Resource->Windows.IsEmpty()) return Complete();
				for (const auto& Window : Resource->Windows)
				{
					if (!IsCurrent(Window, Current))
						return Pending(TEXT("asset_editor_identity_changed"),
							TEXT("The managed editor changed identity without a confirmed replacement. Ownership is retained until its closure can be confirmed."));
					for (auto* Other : Editors->GetAllEditedAssets())
						if (Other && Other->IsAsset() && !Window.Assets.Contains(Other)
							&& Editors->FindEditorsForAsset(Other).Contains(Window.Editor))
							return Pending(TEXT("asset_editor_ownership_changed"),
								TEXT("The managed editor also contains an asset added outside this ownership scope. Separate or close that editor, then resume cleanup."),
								Other->GetPathName());
				}
				if (bTryClosePending)
				{
					bTryClosePending = false;
					const auto Windows = Resource->Windows;
					for (const auto& Window : Windows)
					{
						bool bOtherHolder = false;
						for (const auto& Other : Owner->Resources)
							if (Other != Resource && Other->Users.GetValue() > 0)
								for (const auto& OtherWindow : Other->Windows)
									bOtherHolder |= OtherWindow.Editor == Window.Editor && IsCurrent(OtherWindow, Current);
						if (bOtherHolder)
						{
							Resource->Windows.RemoveAll([&](const FWindow& Item) { return Item.Editor == Window.Editor; });
							continue;
						}
						if (!IsCurrent(Window, Editors->GetAllOpenEditors())) continue;
						// A dirty native editor may decline. Use its conservative
						// unattended dialog default instead of blocking the game
						// thread or silently saving/discarding user work.
						TGuardValue<bool> Unattended(GIsRunningUnattendedScript, true);
						Window.Editor->CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed);
					}
				}
				Current = Editors->GetAllOpenEditors();
				Resource->Windows.RemoveAll([&](const FWindow& Window) { return !Current.Contains(Window.Editor); });
				if (Resource->Windows.IsEmpty()) return Complete();
				return Pending(TEXT("asset_editor_close_unconfirmed"),
					TEXT("The managed asset editor is still open. Resolve unsaved editor changes or close it, then recover this operation and close again."));
			}
		private:
			TSharedRef<FTracker, ESPMode::ThreadSafe> Owner;
			TSharedPtr<FResource, ESPMode::ThreadSafe> Resource;
			bool bComplete = false;
			bool bTryClosePending = false;
		};
	}

	TUniquePtr<IAssetEditorOpenScope> BeginAssetEditorOpen(UObject* Asset)
	{
		check(IsInGameThread());
		return Asset ? MakeUnique<FOpenScope>(Asset, GetTracker()) : nullptr;
	}

	TSharedPtr<IAssetEditorLease, ESPMode::ThreadSafe> AcquireAssetEditorLease(UObject* Asset, bool bAdoptExisting)
	{
		check(IsInGameThread());
		if (!Asset) return nullptr;
		const auto Owner = GetTracker();
		const auto Resource = Owner->FindOrAdd(Asset);
		if (auto* Editors = Owner->Subsystem.Get())
			for (auto* Editor : Editors->FindEditorsForAsset(Asset))
			{
				if (bAdoptExisting)
				{
					Owner->Track(Resource, Editor);
					continue;
				}
				// A multi-asset toolkit can be shared by leases for different
				// assets. Reuse its established ownership, including the original
				// allowed asset set; do not widen it to later user additions.
				const auto Current = Editors->GetAllOpenEditors();
				if (Resource->Windows.ContainsByPredicate([&](const FWindow& Window)
					{ return Window.Editor == Editor; })) continue;
				for (const auto& Other : Owner->Resources)
				{
					const auto* Window = Other->Windows.FindByPredicate([&](const FWindow& Item)
						{ return Item.Editor == Editor && IsCurrent(Item, Current); });
					if (Window)
					{
						Resource->Windows.Add(*Window);
						Resource->bWasManaged = true;
						break;
					}
				}
			}
		return MakeShared<FLease, ESPMode::ThreadSafe>(Owner, Resource);
	}

	void StartAssetEditorTracking() { GetTracker(); }
	void DetachAssetEditorTracking() { if (Tracker) Tracker->Detach(); }
	void ShutdownAssetEditorTracking() { Tracker.Reset(); }

#if WITH_DEV_AUTOMATION_TESTS
	// RunSmokeTests owns a separate process and UserDir. Use its initialized
	// subsystem; a bare subsystem lacks the recent-assets initialization needed
	// by native close. Refuse to write fixture state to ordinary user config.
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetEditorCleanupTest,
		"UeAgentInterface.AssetEditor.Lifetime.Cleanup",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FAssetEditorCleanupTest::RunTest(const FString&)
	{
		FString FixtureUserDir;
		if (!GEditor || !FParse::Value(FCommandLine::Get(), TEXT("UserDir="), FixtureUserDir)
			|| !FPaths::IsUnderDirectory(FPaths::ConvertRelativePathToFull(GEditorPerProjectIni),
				FPaths::ConvertRelativePathToFull(FixtureUserDir)))
		{
			AddError(TEXT("Run this fixture through RunSmokeTests with its isolated UserDir/config."));
			return false;
		}
		auto* Editors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
		if (!Editors || !Editors->GetAllOpenEditors().IsEmpty())
		{
			AddError(TEXT("The isolated fixture requires an initialized subsystem without existing editors."));
			return false;
		}
		TStrongObjectPtr<UCurveFloat> Asset(NewObject<UCurveFloat>(GetTransientPackage(), NAME_None, RF_Transient));
		class FTestEditor final : public IAssetEditorInstance
		{
		public:
			explicit FTestEditor(UAssetEditorSubsystem* InEditors) : Editors(InEditors) {}
			FName GetEditorName() const override { return TEXT("UAIIsolatedCleanupTest"); }
			void FocusWindow(UObject*) override {}
			bool IsPrimaryEditor() const override { return true; }
			void InvokeTab(const FTabId&) override {}
			TSharedPtr<FTabManager> GetAssociatedTabManager() override { return nullptr; }
			double GetLastActivationTime() override { return 0; }
			void RemoveEditingAsset(UObject*) override {}
			bool IncludeAssetInRestoreOpenAssetsPrompt(UObject*) const override { return false; }
			bool CloseWindow(EAssetEditorCloseReason) override
			{
				++Attempts;
				if (bAllowClose) Editors->NotifyEditorClosed(this);
				return true; // Match UE toolkit's non-evidentiary return value.
			}
			UAssetEditorSubsystem* Editors;
			int32 Attempts = 0;
			bool bAllowClose = false;
		} Editor(Editors);
		Editors->NotifyAssetOpened(Asset.Get(), &Editor);
		const auto Owner = MakeShared<FTracker, ESPMode::ThreadSafe>();
		Owner->Subsystem = Editors;
		Owner->Bind();
		{
			FOpenScope FailedOpen(Asset.Get(), Owner);
			FailedOpen.Complete(nullptr);
			TestEqual(TEXT("An unfinished open scope retains its resource"), Owner->Resources.Num(), 1);
		}
		TestTrue(TEXT("A failed open without windows leaves no resource"), Owner->Resources.IsEmpty());
		{
			auto Abandoned = MakeShared<FLease, ESPMode::ThreadSafe>(Owner, Owner->FindOrAdd(Asset.Get()));
		}
		TestTrue(TEXT("An abandoned borrowed lease leaves no empty resource"), Owner->Resources.IsEmpty());
		auto Resource = Owner->FindOrAdd(Asset.Get());
		auto Borrowed = MakeShared<FLease, ESPMode::ThreadSafe>(Owner, Resource);
		UE::UAI::Contract::FDiagnostic Diagnostic;
		TSharedPtr<FJsonObject> Details;
		TestTrue(TEXT("Borrowed editor releases without closing user window"), Borrowed->Close(true, Diagnostic, Details));
		TestEqual(TEXT("Borrowed close never invokes native close"), Editor.Attempts, 0);

		Resource = Owner->FindOrAdd(Asset.Get());
		Owner->Track(Resource, &Editor);
		auto First = MakeShared<FLease, ESPMode::ThreadSafe>(Owner, Resource);
		auto Last = MakeShared<FLease, ESPMode::ThreadSafe>(Owner, Resource);
		TestTrue(TEXT("First shared holder can finish"), First->Close(true, Diagnostic, Details));
		TestEqual(TEXT("Shared resource remains open for last holder"), Editor.Attempts, 0);
		TestFalse(TEXT("True close return with remaining window is pending"), Last->Close(true, Diagnostic, Details));
		TestEqual(TEXT("Specific remaining-window reason"), Diagnostic.Code, FString(TEXT("asset_editor_close_unconfirmed")));
		TestFalse(TEXT("Pending cleanup is not complete"), Details->GetBoolField(TEXT("cleanup_complete")));
		TestEqual(TEXT("Last holder remains responsible"), Resource->Users.GetValue(), 1);
		TestFalse(TEXT("Observation cannot manufacture closure"), Last->Close(false, Diagnostic, Details));
		TestEqual(TEXT("Observation does not repeat close requests"), Editor.Attempts, 1);
		Editor.bAllowClose = true;
		TestTrue(TEXT("Explicit retry confirms native deregistration"), Last->Close(true, Diagnostic, Details));
		TestTrue(TEXT("Completion follows native state"), Editors->GetAllOpenEditors().IsEmpty());
		TestEqual(TEXT("No leaked holders"), Resource->Users.GetValue(), 0);
		TestTrue(TEXT("Completed isolated resources are removed"), Owner->Resources.IsEmpty());

		Editors->NotifyAssetOpened(Asset.Get(), &Editor);
		Resource = Owner->FindOrAdd(Asset.Get());
		Owner->Track(Resource, &Editor);
		auto ReloadLease = MakeShared<FLease, ESPMode::ThreadSafe>(Owner, Resource);
		Owner->Reload(EPackageReloadPhase::PreBatch, nullptr);
		Resource->bAwaitingReopen = true;
		Editors->NotifyEditorClosed(&Editor);
		Owner->Reload(EPackageReloadPhase::PostBatchPreGC, nullptr);
		TestTrue(TEXT("Confirmed native removal clears old pointer before reuse"), Resource->Windows.IsEmpty());
		TestTrue(TEXT("Old pointer is absent from pre-reload snapshot"), Owner->BeforeReload.IsEmpty());
		Editors->NotifyAssetOpened(Asset.Get(), &Editor);
		TestEqual(TEXT("Reopened instance at reused address is owned"), Resource->Windows.Num(), 1);
		Owner->Reload(EPackageReloadPhase::PostBatchPostGC, nullptr);
		Owner->Reload(EPackageReloadPhase::PreBatch, nullptr);
		TestFalse(TEXT("Next native batch settles prior replacement first"), Resource->bAwaitingReopen);
		TestFalse(TEXT("Prior deferred callback is removed before next batch"), Owner->SettleHandle.IsValid());
		TestEqual(TEXT("Ownership survives consecutive synchronous batches"), Resource->Windows.Num(), 1);
		Owner->Reload(EPackageReloadPhase::PostBatchPostGC, nullptr);
		FTSTicker::GetCoreTicker().RemoveTicker(Owner->SettleHandle);
		Owner->SettleReload(0);
		TestTrue(TEXT("Reopened resource closes after batch settlement"), ReloadLease->Close(true, Diagnostic, Details));

		TStrongObjectPtr<UCurveFloat> OtherAsset(NewObject<UCurveFloat>(GetTransientPackage(), NAME_None, RF_Transient));
		Editors->NotifyAssetOpened(Asset.Get(), &Editor);
		Resource = Owner->FindOrAdd(Asset.Get());
		Owner->Track(Resource, &Editor);
		auto ExternalCloseLease = MakeShared<FLease, ESPMode::ThreadSafe>(Owner, Resource);
		Editors->NotifyAssetOpened(OtherAsset.Get(), &Editor);
		Editors->NotifyAssetClosed(Asset.Get(), &Editor);
		TestEqual(TEXT("Closing one registration retains a multi-asset toolkit"), Resource->Windows.Num(), 1);
		Editors->NotifyEditorClosed(&Editor);
		TestTrue(TEXT("External final close removes the old instance immediately"), Resource->Windows.IsEmpty());
		Editors->NotifyAssetOpened(OtherAsset.Get(), &Editor);
		const int32 AttemptsBeforeUserReopen = Editor.Attempts;
		TestTrue(TEXT("Old lease completes after external close"), ExternalCloseLease->Close(true, Diagnostic, Details));
		TestEqual(TEXT("Old lease never closes a user's new instance at the same address"),
			Editor.Attempts, AttemptsBeforeUserReopen);
		TestTrue(TEXT("User-reopened window stays registered"), Editors->GetAllOpenEditors().Contains(&Editor));
		Editors->NotifyEditorClosed(&Editor);

		Resource = Owner->FindOrAdd(Asset.Get());
		auto RenamedLease = MakeShared<FLease, ESPMode::ThreadSafe>(Owner, Resource);
		const FString Renamed = MakeUniqueObjectName(GetTransientPackage(), UCurveFloat::StaticClass(),
			TEXT("UAIAssetEditorRenamed")).ToString();
		TestTrue(TEXT("Fixture object can be renamed"), Asset->Rename(*Renamed, nullptr,
			REN_DontCreateRedirectors | REN_NonTransactional));
		TestEqual(TEXT("Lease readback uses the current object path"),
			RenamedLease->Describe()->GetStringField(TEXT("asset_path")), Asset->GetPathName());
		TestTrue(TEXT("Renamed borrowed lease closes"), RenamedLease->Close(true, Diagnostic, Details));

		{
			auto Open = BeginAssetEditorOpen(Asset.Get());
			Editors->NotifyAssetOpened(Asset.Get(), &Editor);
			Editors->NotifyAssetOpened(OtherAsset.Get(), &Editor);
			Open->Complete(Asset.Get());
		}
		auto AssetLease = AcquireAssetEditorLease(Asset.Get(), false);
		auto OtherAssetLease = AcquireAssetEditorLease(OtherAsset.Get(), false);
		const int32 AttemptsBeforeSharedClose = Editor.Attempts;
		TestTrue(TEXT("First cross-asset lease transfers toolkit responsibility"), AssetLease->Close(true, Diagnostic, Details));
		TestEqual(TEXT("Other asset holder keeps the shared toolkit"), Editor.Attempts, AttemptsBeforeSharedClose);
		TestTrue(TEXT("Last cross-asset lease closes the shared toolkit"), OtherAssetLease->Close(true, Diagnostic, Details));
		TestTrue(TEXT("Shared toolkit's resources are fully retired"), GetTracker()->Resources.IsEmpty());
		TestTrue(TEXT("Isolated owner has no empty records left"), Owner->Resources.IsEmpty());
		if (Editors->GetAllOpenEditors().Contains(&Editor)) Editors->NotifyEditorClosed(&Editor);
		Owner->Detach();
		return true;
	}
#endif
}

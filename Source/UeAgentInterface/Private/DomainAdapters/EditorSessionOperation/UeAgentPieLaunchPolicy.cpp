// Copyright Epic Games, Inc. All Rights Reserved.

#include "DomainAdapters/EditorSessionOperation/UeAgentPieLaunchPolicy.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"

#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Editor/UnrealEdEngine.h"
#include "Engine/Blueprint.h"
#include "Features/IModularFeatures.h"
#include "IPIEBlueprintCompileErrorHandler.h"
#include "PlayInEditorDataTypes.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "UnrealEdGlobals.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UObjectGlobals.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

namespace UE::UAI::DomainAdapters::EditorSessionOperation
{
	struct FPieLaunchPolicy::FImpl final : IPIEBlueprintCompileErrorHandler
	{
		TStrongObjectPtr<ULevelEditorPlaySettings> Settings;
		FCompletion Completion;
		Contract::FDiagnostic Diagnostic;
		FDelegateHandle CancelHandle;
		FDelegateHandle StartedHandle;
		bool bRegistered = false;

		~FImpl() { Disarm(); }

		void Disarm()
		{
			check(IsInGameThread());
			if (bRegistered)
			{
				IModularFeatures::Get().UnregisterModularFeature(GetModularFeatureName(), this);
				bRegistered = false;
			}
			FEditorDelegates::CancelPIE.Remove(CancelHandle);
			FEditorDelegates::PostPIEStarted.Remove(StartedHandle);
			CancelHandle.Reset();
			StartedHandle.Reset();
		}

		void Complete(const bool bStarted)
		{
			if (!GUnrealEd) return;
			const auto Queued = GUnrealEd->GetPlaySessionRequest();
			const auto Active = GUnrealEd->GetPlayInEditorSessionInfo();
			const bool bOwnsQueued = Queued.IsSet() && Queued->EditorPlaySettings == Settings.Get();
			const bool bOwnsActive = bStarted && Active.IsSet()
				&& Active->OriginalRequestParams.EditorPlaySettings == Settings.Get();
			if (!bOwnsQueued && !bOwnsActive) return;
			Disarm();
			if (!bStarted && Diagnostic.IsEmpty())
			{
				Diagnostic = Contract::FDiagnostic::Failure(TEXT("pie_launch_cancelled"),
					TEXT("The native editor cancelled the PIE launch before a play world became active."), TEXT("pie_launch"));
				Diagnostic.SourceKind = TEXT("native_result");
			}
			// CancelPIE is broadcast before UnrealEd resets its queued/session state.
			// Defer consumers so their cleanup cannot reenter that native cancellation.
			if (Completion)
				AsyncTask(ENamedThreads::GameThread,
					[Callback = MoveTemp(Completion), Error = Diagnostic, bStarted]() { Callback(bStarted, Error); });
		}

		bool CancelOnBlueprintCompileErrors(const FRequestPlaySessionParams& Request,
			const TArray<UBlueprint*>& ErroredBlueprints) override
		{
			if (Request.EditorPlaySettings != Settings.Get()) return false;
			TArray<UBlueprint*> Failures = ErroredBlueprints;
			TMap<const UBlueprint*, FString> ResidentPaths;
			// Native PIE scans loaded Blueprints. An earlier manual "Play anyway"
			// suppresses its warning, but must not silently permit UAI to run errors.
			for (TObjectIterator<UBlueprint> It; It; ++It)
			{
				UBlueprint* Blueprint = *It;
				if (IsValid(Blueprint) && Blueprint->Status == BS_Error) Failures.AddUnique(Blueprint);
				// The native existing-error branch also reports garbage objects. They
				// can still be resident, so preserve their identity before filtering
				// operations that require a valid authoring target. Never dereference
				// a raw error pointer that is no longer in the live object iterator.
				if (Failures.Contains(Blueprint)) ResidentPaths.Add(Blueprint, Blueprint->GetPathName());
			}
			if (Failures.IsEmpty()) return false;
			Diagnostic = Contract::FDiagnostic::Failure(TEXT("pie_blueprint_compile_failed"),
				TEXT("PIE was automatically cancelled because Blueprint compilation failed."), TEXT("compile"));
			Diagnostic.SourceKind = TEXT("native_result");
			Diagnostic.PropertyPath = TEXT("blueprints");
			TArray<TSharedPtr<FJsonValue>> Paths;
			for (UBlueprint* Blueprint : Failures)
			{
				const FString* Path = ResidentPaths.Find(Blueprint);
				if (!Path)
				{
					auto Missing = Contract::FDiagnostic::Failure(TEXT("blueprint_compile_error_identity_unavailable"),
						TEXT("Native PIE reported a Blueprint error whose object was no longer resident when cancellation was handled."), TEXT("compile"));
					Missing.SourceKind = TEXT("native_result");
					Diagnostic.Diagnostics.Add(MoveTemp(Missing));
					continue;
				}
				Paths.Add(MakeShared<FJsonValueString>(*Path));
				auto Failure = UeAgentAuthoringDiagnostics::MakeCompileFailure(
					TEXT("blueprint_compile_failed"), *Path, TArray<TSharedPtr<FJsonValue>>());
				Failure.PublicMessage = TEXT("Blueprint has unresolved native compiler errors; PIE was cancelled.");
				if (!IsValid(Blueprint))
				{
					Failure.PublicMessage = TEXT("Native PIE reported compilation errors on a resident Blueprint that is no longer valid; its identity was preserved.");
					Failure.PropertyPath = TEXT("native_object_valid");
					Failure.ActualValuePresent = true;
					Failure.ActualValue = MakeShared<FJsonValueBoolean>(false);
					Diagnostic.Diagnostics.Add(MoveTemp(Failure));
					continue;
				}
				ContentGraphRead::FUeAgentContentGraphCapture Capture;
				Capture.Owner = Blueprint;
				ContentGraphRead::FUeAgentContentGraphReadProductionEnvironment Environment;
				Contract::FDiagnostic CaptureError;
				if (Environment.TryCaptureDiagnostics(Capture, CaptureError))
				{
					for (const auto& Row : Capture.Diagnostics)
					{
						if (Row.Severity != TEXT("error")) continue;
						auto Item = Contract::FDiagnostic::Failure(Row.Code, Row.Message, TEXT("compile"));
						Item.TargetPath = *Path;
						Item.SubjectId = Row.OwnerId;
						Item.SourceKind = TEXT("native_result");
						Failure.Diagnostics.Add(MoveTemp(Item));
					}
				}
				else Failure.Append(CaptureError);
				Diagnostic.Diagnostics.Add(MoveTemp(Failure));
			}
			Diagnostic.ActualValuePresent = true;
			Diagnostic.ActualValue = MakeShared<FJsonValueArray>(MoveTemp(Paths));
			// Only record a decision here. UnrealEd owns EndPIE/navigation/CancelPIE.
			return true;
		}
	};

	FPieLaunchPolicy::FPieLaunchPolicy() : Impl(MakeUnique<FImpl>()) {}
	FPieLaunchPolicy::~FPieLaunchPolicy()
	{
		// Native sessions can be released by the operation worker after Join.
		FImpl* Owned = Impl.Release();
		if (IsInGameThread()) delete Owned;
		else AsyncTask(ENamedThreads::GameThread, [Owned]() { delete Owned; });
	}

	bool FPieLaunchPolicy::RequestPlaySession(FRequestPlaySessionParams& Params, FCompletion Completion,
		Contract::FDiagnostic& Error)
	{
		check(IsInGameThread());
		Impl->Disarm();
		if (!GUnrealEd)
		{
			Error = Contract::FDiagnostic::Failure(TEXT("pie_editor_unavailable"),
				TEXT("The native editor is unavailable to queue PIE."), TEXT("pie_launch"));
			return false;
		}
		Impl->Settings.Reset(DuplicateObject<ULevelEditorPlaySettings>(Params.EditorPlaySettings
			? Params.EditorPlaySettings.Get() : GetMutableDefault<ULevelEditorPlaySettings>(), GetTransientPackage()));
		if (!Impl->Settings.IsValid())
		{
			Error = Contract::FDiagnostic::Failure(TEXT("pie_play_settings_create_failed"),
				TEXT("The PIE launch could not create request-local play settings."), TEXT("pie_launch"));
			return false;
		}
		Impl->Settings->AutoRecompileBlueprints = true;
		Params.EditorPlaySettings = Impl->Settings.Get();
		Impl->Completion = MoveTemp(Completion);
		Impl->Diagnostic.Reset();
		IModularFeatures::Get().RegisterModularFeature(FImpl::GetModularFeatureName(), Impl.Get());
		Impl->bRegistered = true;
		Impl->CancelHandle = FEditorDelegates::CancelPIE.AddRaw(Impl.Get(), &FImpl::Complete, false);
		Impl->StartedHandle = FEditorDelegates::PostPIEStarted.AddLambda([State = Impl.Get()](bool) { State->Complete(true); });
		GUnrealEd->RequestPlaySession(Params);
		// RequestPlaySession duplicates the supplied settings. Bind the actual
		// native request now, before the queued request can run on the next tick.
		const auto Queued = GUnrealEd->GetPlaySessionRequest();
		if (!Queued.IsSet())
		{
			Impl->Disarm();
			Error = Contract::FDiagnostic::Failure(TEXT("pie_request_not_queued"),
				TEXT("The native editor did not retain the requested PIE launch."), TEXT("pie_launch"));
			return false;
		}
		Impl->Settings.Reset(Queued->EditorPlaySettings.Get());
		return true;
	}

	void FPieLaunchPolicy::Disarm() { Impl->Disarm(); }
}

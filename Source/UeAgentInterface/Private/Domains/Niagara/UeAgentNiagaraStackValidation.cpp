// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Niagara/UeAgentNiagaraStackValidation.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "NiagaraEmitter.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemEditorData.h"
#include "NiagaraSystemFactoryNew.h"
#include "ViewModels/NiagaraEmitterHandleViewModel.h"
#include "ViewModels/NiagaraSystemViewModel.h"
#include "ViewModels/Stack/NiagaraStackEntry.h"
#include "ViewModels/Stack/NiagaraStackViewModel.h"

namespace UeAgentNiagaraStackValidation
{
	namespace
	{
		UE::UAI::Contract::FDiagnostic NativeStackFailure(const TCHAR* Code, const FString& Reason,
			UObject* Asset, const FString& Field, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("verification"));
			Error.SourceKind = TEXT("native_result");
			if (Asset && Asset->IsAsset()) Error.TargetPath = Asset->GetPathName();
			else if (Asset) Error.InternalContext = Asset->GetPathName();
			Error.PropertyPath = Field;
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			return Error;
		}

		void AppendStackIssues(
			UNiagaraStackViewModel* StackViewModel,
			FResult& OutResult,
			UObject* Asset,
			const bool bIncludeIssues,
			const FString& EmitterHandleId = FString(),
			const FString& EmitterName = FString())
		{
			if (!StackViewModel)
			{
				auto Error = NativeStackFailure(TEXT("niagara_stack_view_model_missing"),
					TEXT("The native Niagara stack view is unavailable."), Asset, TEXT("stack_view_available"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				Error.CandidateName = EmitterName;
				OutResult.InspectionDiagnostic.Append(Error);
				return;
			}
			UNiagaraStackEntry* RootEntry = StackViewModel->GetRootEntry();
			if (!RootEntry)
			{
				auto Error = NativeStackFailure(TEXT("niagara_stack_root_missing"),
					TEXT("The native Niagara stack has no root entry to inspect."), Asset, TEXT("stack_root_available"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				Error.CandidateName = EmitterName;
				OutResult.InspectionDiagnostic.Append(Error);
				return;
			}
			RootEntry->RefreshChildren();
			RootEntry->RefreshFilteredChildren();
			OutResult.ErrorCount += RootEntry->GetTotalNumberOfErrorIssues();
			OutResult.WarningCount += RootEntry->GetTotalNumberOfWarningIssues();
			if (bIncludeIssues || OutResult.ErrorCount > 0 || OutResult.WarningCount > 0)
			{
				TArray<UNiagaraStackEntry*> Entries{RootEntry};
				Entries.Append(RootEntry->GetAllChildrenWithIssues());
				TSet<const UNiagaraStackEntry*> Seen;
				for (const UNiagaraStackEntry* Entry : Entries)
				{
					if (!Entry || Seen.Contains(Entry)) continue;
					Seen.Add(Entry);
					for (const UNiagaraStackEntry::FStackIssue& NativeIssue : Entry->GetIssues())
					{
						if (!bIncludeIssues && NativeIssue.GetSeverity() != EStackIssueSeverity::Error
							&& NativeIssue.GetSeverity() != EStackIssueSeverity::Warning) continue;
						FIssue& Issue = OutResult.Issues.AddDefaulted_GetRef();
						Issue.Id = NativeIssue.GetUniqueIdentifier();
						const FString ShortReason = NativeIssue.GetShortDescription().ToString();
						const FString LongReason = NativeIssue.GetLongDescription().ToString();
						Issue.Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("niagara_stack_issue"),
							ShortReason.IsEmpty() && LongReason.IsEmpty() ? TEXT("Unreal reported a Niagara stack issue without a description.")
							: LongReason.IsEmpty() || LongReason == ShortReason ? ShortReason
							: ShortReason.IsEmpty() ? LongReason : ShortReason + TEXT(": ") + LongReason, TEXT("verification"));
						Issue.Diagnostic.Severity = NativeIssue.GetSeverity() == EStackIssueSeverity::Error ? UE::UAI::Contract::ESeverity::Error
							: NativeIssue.GetSeverity() == EStackIssueSeverity::Warning ? UE::UAI::Contract::ESeverity::Warning : UE::UAI::Contract::ESeverity::Info;
						Issue.Diagnostic.SourceKind = TEXT("native_stack_issue");
						if (Asset && Asset->IsAsset()) Issue.Diagnostic.TargetPath = Asset->GetPathName();
						Issue.EntryKey = Entry->GetStackEditorDataKey();
						Issue.EntryName = Entry->GetDisplayName().ToString();
						Issue.Diagnostic.SubjectId = Issue.EntryName;
						Issue.Diagnostic.CandidateName = Issue.EntryName;
						Issue.EmitterHandleId = EmitterHandleId;
						Issue.EmitterName = EmitterName;
						auto Source = MakeShared<FJsonObject>();
						Source->SetStringField(TEXT("source_kind"), TEXT("native_stack_issue"));
						Source->SetStringField(TEXT("entry_name"), Issue.EntryName);
						Source->SetStringField(TEXT("emitter_name"), EmitterName);
						Issue.Diagnostic.RelatedSources.Add(MakeShared<FJsonValueObject>(Source));
						for (const UNiagaraStackEntry::FStackIssueFix& Fix : NativeIssue.GetFixes())
						{
							Issue.Fixes.Add({Fix.GetUniqueIdentifier(), Fix.GetDescription().ToString()});
							if (!Issue.Diagnostic.SuggestedFix.IsEmpty()) Issue.Diagnostic.SuggestedFix += TEXT("\n");
							Issue.Diagnostic.SuggestedFix += Fix.GetDescription().ToString();
						}
					}
				}
			}

		}

		TSharedPtr<FNiagaraSystemViewModel> CreateSystemViewModel(
			UNiagaraSystem& System,
			const ENiagaraSystemViewModelEditMode EditMode)
		{
			TSharedPtr<FNiagaraSystemViewModel> ViewModel =
				MakeShared<FNiagaraSystemViewModel>();
			FNiagaraSystemViewModelOptions Options;
			Options.bCanModifyEmittersFromTimeline = false;
			Options.bCanAutoCompile = false;
			Options.bCanSimulate = false;
			Options.bIsForDataProcessingOnly = true;
			Options.EditMode = EditMode;
			Options.MessageLogGuid = System.GetAssetGuid();
			ViewModel->Initialize(System, Options);
			return ViewModel;
		}
	}

	UE::UAI::Contract::FDiagnostic FResult::GetFailure(bool bIncludeWarnings) const
	{
		if (!InspectionDiagnostic.IsEmpty()) return InspectionDiagnostic;
		UE::UAI::Contract::FDiagnostic Result;
		for (const FIssue& Issue : Issues)
		{
			if (Issue.Diagnostic.Severity != UE::UAI::Contract::ESeverity::Error
				&& !(bIncludeWarnings && Issue.Diagnostic.Severity == UE::UAI::Contract::ESeverity::Warning)) continue;
			if (Result.IsEmpty())
			{
				Result = UE::UAI::Contract::FDiagnostic::Failure(TEXT("niagara_stack_validation_failed"),
					TEXT("The Niagara stack contains issues that prevent this operation from completing."), TEXT("verification"));
				Result.SourceKind = TEXT("native_result");
				Result.TargetPath = Issue.Diagnostic.TargetPath;
			}
			Result.Diagnostics.Add(Issue.Diagnostic);
		}
		return Result;
	}

	FResult InspectAsset(UObject* Asset, const bool bIncludeIssues)
	{
		FResult Result;
		if (UNiagaraSystem* System = Cast<UNiagaraSystem>(Asset))
		{
			const TSharedPtr<FNiagaraSystemViewModel> ViewModel =
				CreateSystemViewModel(
					*System,
					ENiagaraSystemViewModelEditMode::SystemAsset);
			if (!ViewModel.IsValid())
			{
				Result.InspectionDiagnostic = NativeStackFailure(TEXT("niagara_system_stack_view_model_create_failed"),
					TEXT("Unreal could not create the system view model required to inspect the stack."), Asset, TEXT("system_view_model_created"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				return Result;
			}
			AppendStackIssues(
				ViewModel->GetSystemStackViewModel(),
				Result, Asset, bIncludeIssues);
			for (const TSharedRef<FNiagaraEmitterHandleViewModel>& Handle :
				ViewModel->GetEmitterHandleViewModels())
			{
				AppendStackIssues(
					Handle->GetEmitterStackViewModel(),
					Result, Asset, bIncludeIssues, Handle->GetId().ToString(EGuidFormats::DigitsWithHyphens),
					Handle->GetName().ToString());
			}
			Result.bInspectionComplete = Result.InspectionDiagnostic.IsEmpty();
			return Result;
		}

		if (UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(Asset))
		{
			UNiagaraSystem* TransientSystem = NewObject<UNiagaraSystem>(
				GetTransientPackage(),
				NAME_None,
				RF_Transient | RF_Transactional);
			if (!TransientSystem)
			{
				Result.InspectionDiagnostic = NativeStackFailure(TEXT("niagara_emitter_stack_transient_system_failed"),
					TEXT("Unreal could not create the temporary system required to inspect the emitter stack."), Asset, TEXT("inspection_system_created"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				return Result;
			}
			UNiagaraSystemFactoryNew::InitializeSystem(
				TransientSystem,
				true);
			TransientSystem->EnsureFullyLoaded();
			Emitter->UpdateEmitterAfterLoad();

			const FGuid VersionGuid =
				Emitter->IsVersioningEnabled()
					&& Emitter->VersionToOpenInEditor.IsValid()
				? Emitter->VersionToOpenInEditor
				: Emitter->GetExposedVersion().VersionGuid;
			const TSharedPtr<FNiagaraSystemViewModel> ViewModel =
				CreateSystemViewModel(
					*TransientSystem,
					ENiagaraSystemViewModelEditMode::EmitterAsset);
			if (!ViewModel.IsValid())
			{
				Result.InspectionDiagnostic = NativeStackFailure(TEXT("niagara_emitter_stack_view_model_create_failed"),
					TEXT("Unreal could not create the emitter view model required to inspect the stack."), Asset, TEXT("emitter_view_model_created"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				return Result;
			}
			ViewModel->GetEditorData().SetOwningSystemIsPlaceholder(
				true,
				*TransientSystem);
			ViewModel->AddEmitter(
				FVersionedNiagaraEmitter(Emitter, VersionGuid));
			const TArray<TSharedRef<FNiagaraEmitterHandleViewModel>>& Handles =
				ViewModel->GetEmitterHandleViewModels();
			if (Handles.IsEmpty())
			{
				Result.InspectionDiagnostic = NativeStackFailure(TEXT("niagara_emitter_stack_view_model_missing_handle"),
					TEXT("The inspection system did not create a handle for the emitter."), Asset, TEXT("emitter_handle_count"),
					MakeShared<FJsonValueNumber>(Handles.Num()), MakeShared<FJsonValueString>(TEXT("at least one emitter handle")));
				return Result;
			}
			AppendStackIssues(
				Handles[0]->GetEmitterStackViewModel(),
				Result, Asset, bIncludeIssues);
			Result.bInspectionComplete = Result.InspectionDiagnostic.IsEmpty();
			return Result;
		}

		if (Asset && Asset->IsA<UNiagaraScript>())
		{
			Result.bInspectionComplete = Result.InspectionDiagnostic.IsEmpty();
			return Result;
		}

		Result.InspectionDiagnostic = NativeStackFailure(TEXT("niagara_stack_target_invalid"),
			TEXT("Stack inspection requires a Niagara system, emitter or script."), Asset, TEXT("asset.class"),
			Asset ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Asset->GetClass()->GetName())) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()),
			MakeShared<FJsonValueString>(TEXT("NiagaraSystem, NiagaraEmitter or NiagaraScript")));
		return Result;
	}
}

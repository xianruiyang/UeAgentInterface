// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Niagara/UeAgentNiagaraStackValidation.h"

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
		void AppendStackIssues(
			UNiagaraStackViewModel* StackViewModel,
			FResult& OutResult)
		{
			if (!StackViewModel)
			{
				return;
			}
			UNiagaraStackEntry* RootEntry = StackViewModel->GetRootEntry();
			if (!RootEntry)
			{
				return;
			}
			RootEntry->RefreshChildren();
			RootEntry->RefreshFilteredChildren();
			OutResult.ErrorCount += RootEntry->GetTotalNumberOfErrorIssues();
			OutResult.WarningCount += RootEntry->GetTotalNumberOfWarningIssues();
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

	FResult InspectAsset(UObject* Asset)
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
				Result.FirstError =
					TEXT("niagara_system_stack_view_model_create_failed");
				return Result;
			}
			AppendStackIssues(
				ViewModel->GetSystemStackViewModel(),
				Result);
			for (const TSharedRef<FNiagaraEmitterHandleViewModel>& Handle :
				ViewModel->GetEmitterHandleViewModels())
			{
				AppendStackIssues(
					Handle->GetEmitterStackViewModel(),
					Result);
			}
			Result.bInspectionComplete = true;
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
				Result.FirstError =
					TEXT("niagara_emitter_stack_transient_system_failed");
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
				Result.FirstError =
					TEXT("niagara_emitter_stack_view_model_create_failed");
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
				Result.FirstError =
					TEXT("niagara_emitter_stack_view_model_missing_handle");
				return Result;
			}
			AppendStackIssues(
				Handles[0]->GetEmitterStackViewModel(),
				Result);
			Result.bInspectionComplete = true;
			return Result;
		}

		if (Asset && Asset->IsA<UNiagaraScript>())
		{
			Result.bInspectionComplete = true;
			return Result;
		}

		Result.FirstError = TEXT("niagara_stack_target_invalid");
		return Result;
	}
}

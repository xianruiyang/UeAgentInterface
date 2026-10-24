#include "Composition/HjsonImport/UeAgentPreviewWorkspaceApply.h"

#include "AssetViewerSettings.h"
#include "Composition/HjsonImport/UeAgentSemanticWorkspaceImport.h"
#include "Core/AssetPreview/UeAgentAssetPreviewState.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/TextureCube.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

namespace UE::UAI::Composition::HjsonImport
{
	namespace
	{
		UE::UAI::Contract::FDiagnostic PreviewFailure(
			const FString& Code, const FString& Reason, const FString& Stage,
			const FString& Field, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, Stage);
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = Field;
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			Error.ActualValuePresent = true;
			Error.ExpectedValuePresent = true;
			return Error;
		}

		UE::UAI::Contract::FDiagnostic MissingPreviewOwner(
			const FString& Code, const FString& Reason, const FString& Stage, const FString& Field)
		{
			return PreviewFailure(Code, Reason, Stage, Field,
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
		}

		UE::UAI::Contract::FDiagnostic ProfileIdentityFailure(
			const UAssetViewerSettings& Settings, int32 Index, const FString& Name,
			const FString& Stage)
		{
			if (!Settings.Profiles.IsValidIndex(Index))
			{
				return PreviewFailure(TEXT("preview_profile_identity_changed"),
					TEXT("The requested preview profile index no longer exists"), Stage, TEXT("preview_profile_index"),
					MakeShared<FJsonValueNumber>(Index),
					MakeShared<FJsonValueString>(FString::Printf(TEXT("index in [0, %d)"), Settings.Profiles.Num())));
			}
			if (Settings.Profiles[Index].ProfileName != Name)
			{
				return PreviewFailure(TEXT("preview_profile_identity_changed"),
					TEXT("The preview profile at the requested index has a different name"), Stage, TEXT("preview_profile"),
					MakeShared<FJsonValueString>(Settings.Profiles[Index].ProfileName), MakeShared<FJsonValueString>(Name));
			}
			return MissingPreviewOwner(TEXT("preview_profile_identity_changed"),
				TEXT("The preview profile storage was replaced after preparation"), Stage, TEXT("prepared_profile_storage_matches"));
		}

		void SetPreviewFailure(HjsonWorkspace::FWorkspaceError& Error, UE::UAI::Contract::FDiagnostic Diagnostic,
			const FString& SourceUnit, const FString& Stage,
			const FString& Target, const TArray<TSharedPtr<FJsonValue>>& Descriptors,
			const TFunction<void(HjsonWorkspace::FWorkspaceError&)>& Locate,
			bool bAppend = false)
		{
			if (Diagnostic.Stage.IsEmpty()) Diagnostic.Stage = Stage;
			if (Diagnostic.TargetPath.IsEmpty()) Diagnostic.TargetPath = Target;
			TSharedPtr<FJsonObject> SourceObject;
			for (const auto& Descriptor : Descriptors)
			{
				if (!Descriptor || Descriptor->Type != EJson::Object) continue;
				const auto Object = Descriptor->AsObject();
				FString Path;
				Object->TryGetStringField(TEXT("property_path"), Path);
				if (Descriptors.Num() == 1 || (!Diagnostic.PropertyPath.IsEmpty() && Path == Diagnostic.PropertyPath))
				{ SourceObject = Object; break; }
			}
			if (SourceObject)
			{
				if (Diagnostic.PropertyPath.IsEmpty()) SourceObject->TryGetStringField(TEXT("property_path"), Diagnostic.PropertyPath);
				UeAgentAuthoringDiagnostics::LocateAuthoringSource(Diagnostic, *SourceObject);
			}
			HjsonWorkspace::FWorkspaceError Failure;
			Failure.SetFailure(MoveTemp(Diagnostic), SourceUnit);
			if (Locate) Locate(Failure);
			if (bAppend) Error.Append(Failure);
			else Error = MoveTemp(Failure);
		}
	}

	bool FPreviewWorkspaceApply::Prepare(const FPreparedAuthoringImport& Import,
		FSemanticWorkspaceChangeProjection& Projection, HjsonWorkspace::FWorkspaceError& Error)
	{
		Profiles.Reset();
		Locals.Reset();
		LocateFailure = [Workspace = Import.GetWorkspace()](HjsonWorkspace::FWorkspaceError& Failure)
			{ Failure.LocateUnits(Workspace->GetUnitSources()); };
		TMap<int32, TArray<TSharedPtr<FJsonValue>>> Descriptors;
		TMap<int32, FString> Names;
		TMap<int32, FString> Sources;
		TArray<FString> PreviewUnits;
		for (const auto& UnitId : Projection.ChangedUnitIds)
		{
			const auto* Unit = Import.GetWorkspace()->GetModel()->FindUnit(UnitId);
			if (!Unit || !AssetPreview::IsPreviewSemantic(Unit->GetSemanticTypeRef())) continue;
			const auto* Change = Import.FindChangeForTargetIdentity(Unit->GetTargetIdentityKey());
			const auto* Descriptor = Projection.PropertyChangesByUnitId.Find(UnitId);
			if (!Change || !Descriptor)
			{
				auto Diagnostic = MissingPreviewOwner(TEXT("preview_property_change_missing"),
					!Change ? TEXT("The preview unit has no prepared property change")
						: TEXT("The preview unit has no projected property descriptor"),
					TEXT("preview_prepare"), !Change ? TEXT("property_change_present") : TEXT("property_descriptor_present"));
				Diagnostic.SubjectId = UnitId;
				SetPreviewFailure(Error, MoveTemp(Diagnostic), UnitId, TEXT("preview_prepare"), {}, {}, LocateFailure);
				return false;
			}
			int32 Index = INDEX_NONE;
			const auto& Identity = Change->GetIdentity();
			const FString LocalGroup = Identity.GetSelector(TEXT("preview_local_group"));
			if (!LocalGroup.IsEmpty())
			{
				const auto State = AssetPreview::FindLocalState(Identity.GetSelector(TEXT("asset_path")), Identity.GetSelector(TEXT("preview_target")));
				if (!State)
				{
					auto Diagnostic = MissingPreviewOwner(TEXT("preview_local_owner_unavailable"),
						TEXT("No local preview state exists for the requested asset and preview target"),
						TEXT("preview_prepare"), TEXT("local_preview_state_present"));
					Diagnostic.InternalContext = Identity.GetSelector(TEXT("preview_target"));
					SetPreviewFailure(Error, MoveTemp(Diagnostic), UnitId, TEXT("preview_prepare"), Identity.GetSelector(TEXT("asset_path")), {*Descriptor}, LocateFailure);
					return false;
				}
				FLocalApply* Plan = Locals.FindByPredicate([&](const FLocalApply& Candidate)
					{ return Candidate.State == State && Candidate.Group == LocalGroup; });
				if (!Plan)
				{
					const int32 LocalIndex = Locals.AddDefaulted();
					Plan = &Locals[LocalIndex];
					Plan->State = State;
					Plan->Group = LocalGroup;
					Plan->SourceUnit = UnitId;
				}
				Plan->Descriptors.Add(*Descriptor);
				FString Field = (*Descriptor)->AsObject()->GetStringField(TEXT("property_path"));
				int32 Dot;
				if (Field.FindChar(TEXT('.'), Dot)) Field.LeftInline(Dot);
				Plan->Fields.Add(FName(Field));
				PreviewUnits.Add(UnitId);
				continue;
			}
			if (!LexTryParseString(Index, *Identity.GetSelector(TEXT("preview_profile_index"))) || Index < 0)
			{
				auto Diagnostic = PreviewFailure(TEXT("preview_profile_identity_invalid"),
					TEXT("The preview profile index must be a non-negative integer"), TEXT("preview_prepare"), TEXT("preview_profile_index"),
					MakeShared<FJsonValueString>(Identity.GetSelector(TEXT("preview_profile_index"))),
					MakeShared<FJsonValueString>(TEXT("non-negative integer")));
				SetPreviewFailure(Error, MoveTemp(Diagnostic), UnitId, TEXT("preview_prepare"), Identity.GetSelector(TEXT("asset_path")), {*Descriptor}, LocateFailure);
				return false;
			}
			Names.Add(Index, Identity.GetSelector(TEXT("preview_profile")));
			Sources.FindOrAdd(Index, UnitId);
			Descriptors.FindOrAdd(Index).Add(*Descriptor);
			PreviewUnits.Add(UnitId);
		}
		auto* Settings = UAssetViewerSettings::Get();
		for (const auto& Pair : Descriptors)
		{
			if (!Settings->Profiles.IsValidIndex(Pair.Key)
				|| Settings->Profiles[Pair.Key].ProfileName != Names.FindChecked(Pair.Key))
			{
				SetPreviewFailure(Error, ProfileIdentityFailure(*Settings, Pair.Key, Names.FindChecked(Pair.Key), TEXT("preview_prepare")),
					Sources.FindChecked(Pair.Key), TEXT("preview_prepare"), TEXT("preview_profile:") + Names.FindChecked(Pair.Key), Pair.Value, LocateFailure);
				return false;
			}
			FProfileApply Plan;
			Plan.Index = Pair.Key;
			Plan.Name = Names.FindChecked(Pair.Key);
			Plan.SourceUnit = Sources.FindChecked(Pair.Key);
			Plan.Target = TEXT("preview_profile:") + Plan.Name;
			Plan.Descriptors = Pair.Value;
			Plan.PreparedAddress = &Settings->Profiles[Pair.Key];
			for (const auto& Descriptor : Pair.Value)
			{
				FString Path = Descriptor->AsObject()->GetStringField(TEXT("property_path"));
				int32 Dot;
				if (Path.FindChar(TEXT('.'), Dot)) Path.LeftInline(Dot);
				Plan.Fields.Add(FName(Path));
			}
			UeAgentAuthoringObjectApply::FOptions Options;
			Options.bAllowTemplateOwners = true;
			Options.bEmitEditorNotifications = false;
			Options.bMarkPackageDirty = false;
			Options.bCompactValues = true;
			UeAgentAuthoringObjectApply::FFailure Failure;
			if (!UeAgentAuthoringObjectApply::PrepareStruct(FPreviewSceneProfile::StaticStruct(),
				&Settings->Profiles[Pair.Key], Settings, Pair.Value, Options, Plan.Prepared, Failure))
			{
				SetPreviewFailure(Error, Failure.Diagnostic, Plan.SourceUnit, TEXT("preview_prepare"), Plan.Target, Plan.Descriptors, LocateFailure);
				return false;
			}
			Profiles.Add(MoveTemp(Plan));
		}
		for (auto& Plan : Locals)
		{
			if (!PrepareLocal(Plan, Error)) return false;
		}
		// The asset adapter receives only asset mutations. Preview uses the same
		// parsed descriptors and native property transaction, with its own owner.
		for (const FString& Unit : PreviewUnits)
		{
			Projection.PropertyChangesByUnitId.Remove(Unit);
			Projection.ChangedUnitIds.Remove(Unit);
		}
		return true;
	}

	bool FPreviewWorkspaceApply::PrepareLocal(FLocalApply& Plan, HjsonWorkspace::FWorkspaceError& Error)
	{
		Plan.State->Asset = FindObject<UObject>(nullptr, *Plan.State->AssetPath);
		UScriptStruct* Type;
		void* Data;
		if (!Plan.State->Asset.IsValid() || !AssetPreview::ResolveLocalOwner(Plan.State, Plan.Group, Type, Data))
		{
			auto Diagnostic = MissingPreviewOwner(TEXT("preview_local_owner_unavailable"),
				!Plan.State->Asset.IsValid() ? TEXT("The asset owning the local preview state is no longer available")
					: TEXT("The requested local preview group has no resolvable owner"),
				TEXT("preview_prepare"), !Plan.State->Asset.IsValid() ? TEXT("asset_present") : TEXT("local_group_owner_present"));
			Diagnostic.InternalContext = Plan.Group;
			SetPreviewFailure(Error, MoveTemp(Diagnostic), Plan.SourceUnit, TEXT("preview_prepare"), Plan.State->AssetPath, Plan.Descriptors, LocateFailure);
			return false;
		}
		UeAgentAuthoringObjectApply::FOptions Options;
		Options.bRecordOwnerTransaction = false;
		Options.bAllowTemplateOwners = true;
		Options.bEmitEditorNotifications = false;
		Options.bMarkPackageDirty = false;
		Options.bCompactValues = true;
		UeAgentAuthoringObjectApply::FFailure Failure;
		if (!UeAgentAuthoringObjectApply::PrepareStruct(Type, Data, Plan.State->Asset.Get(),
			Plan.Descriptors, Options, Plan.Prepared, Failure))
		{
			SetPreviewFailure(Error, Failure.Diagnostic, Plan.SourceUnit, TEXT("preview_prepare"), Plan.State->AssetPath, Plan.Descriptors, LocateFailure);
			return false;
		}
		Plan.PreparedOwner = Plan.State->Asset;
		return true;
	}

	bool FPreviewWorkspaceApply::ValidateOwner(const FProfileApply& Plan, HjsonWorkspace::FWorkspaceError& Error,
		const FString& Stage, bool bAppend) const
	{
		auto* Settings = UAssetViewerSettings::Get();
		if (!Settings->Profiles.IsValidIndex(Plan.Index) || Settings->Profiles[Plan.Index].ProfileName != Plan.Name
			|| &Settings->Profiles[Plan.Index] != Plan.PreparedAddress)
		{
			SetPreviewFailure(Error, ProfileIdentityFailure(*Settings, Plan.Index, Plan.Name, Stage),
				Plan.SourceUnit, Stage, Plan.Target, Plan.Descriptors, LocateFailure, bAppend);
			return false;
		}
		return true;
	}

	bool FPreviewWorkspaceApply::Notify(FProfileApply& Plan, HjsonWorkspace::FWorkspaceError& Error,
		const FString& Stage, bool bAppend)
	{
		if (!ValidateOwner(Plan, Error, Stage, bAppend)) return false;
		auto* Settings = UAssetViewerSettings::Get();
		auto& Profile = Settings->Profiles[Plan.Index];
		// PostEditChangeProperty uses the globally selected index for this derived
		// path. Synchronize the actual target instead, without switching user selection.
		if (Plan.Fields.Contains(TEXT("EnvironmentCubeMap")))
			Profile.EnvironmentCubeMapPath = Profile.EnvironmentCubeMap.ToSoftObjectPath().ToString();
		for (const FName Field : Plan.Fields)
			Settings->OnAssetViewerSettingsChanged().Broadcast(Field);
		return true;
	}

	bool FPreviewWorkspaceApply::Apply(HjsonWorkspace::FWorkspaceError& Error)
	{
		for (auto& Plan : Profiles)
		{
			if (!ValidateOwner(Plan, Error, TEXT("preview_apply"))) return false;
			UeAgentAuthoringObjectApply::FApplyResult Result;
			if (!UeAgentAuthoringObjectApply::ApplyPrepared(Plan.Prepared, Result))
			{
				SetPreviewFailure(Error, Result.Diagnostic, Plan.SourceUnit, TEXT("preview_apply"), Plan.Target, Plan.Descriptors, LocateFailure);
				return false;
			}
			if (!Notify(Plan, Error, TEXT("preview_apply"))) return false;
		}
		for (auto& Plan : Locals)
		{
			// Asset lifecycle may replace the UObject after full static preparation.
			// Detached preview storage survives, but the codec context must be rebound.
			if (!Plan.PreparedOwner.IsValid() || Plan.PreparedOwner.Get() != FindObject<UObject>(nullptr, *Plan.State->AssetPath))
				if (!PrepareLocal(Plan, Error)) return false;
			UeAgentAuthoringObjectApply::FApplyResult Result;
			if (!UeAgentAuthoringObjectApply::ApplyPrepared(Plan.Prepared, Result))
			{
				SetPreviewFailure(Error, Result.Diagnostic, Plan.SourceUnit, TEXT("preview_apply"), Plan.State->AssetPath, Plan.Descriptors, LocateFailure);
				return false;
			}
			UE::UAI::Contract::FDiagnostic NativeError;
			if (!AssetPreview::ApplyLocalState(*Plan.State, Plan.Group, Plan.Fields, NativeError))
			{
				SetPreviewFailure(Error, NativeError, Plan.SourceUnit, TEXT("preview_apply"), Plan.State->AssetPath, Plan.Descriptors, LocateFailure);
				return false;
			}
		}
		return Verify(Error);
	}

	bool FPreviewWorkspaceApply::Verify(HjsonWorkspace::FWorkspaceError& Error)
	{
		for (auto& Plan : Locals)
		{
			if (!Plan.PreparedOwner.IsValid())
			{
				SetPreviewFailure(Error, MissingPreviewOwner(TEXT("preview_local_owner_expired"),
					TEXT("The prepared local preview owner expired before verification"), TEXT("preview_verify"), TEXT("prepared_owner_present")),
					Plan.SourceUnit, TEXT("preview_verify"), Plan.State->AssetPath, Plan.Descriptors, LocateFailure);
				UeAgentAuthoringDiagnostics::ReportConsistencyWarning(Error.GetCode(), Error.GetDiagnostic(), TEXT("preview_verify"));
				Error = HjsonWorkspace::FWorkspaceError();
				continue;
			}
			UeAgentAuthoringObjectApply::FApplyResult Result;
			if (!UeAgentAuthoringObjectApply::VerifyPrepared(Plan.Prepared, Result))
			{
				SetPreviewFailure(Error, Result.Diagnostic, Plan.SourceUnit, TEXT("preview_verify"), Plan.State->AssetPath, Plan.Descriptors, LocateFailure);
				UeAgentAuthoringDiagnostics::ReportConsistencyWarning(Error.GetCode(), Error.GetDiagnostic(), TEXT("preview_verify"));
				Error = HjsonWorkspace::FWorkspaceError();
			}
		}
		for (auto& Plan : Profiles)
		{
			if (!ValidateOwner(Plan, Error, TEXT("preview_verify"))) {
				UeAgentAuthoringDiagnostics::ReportConsistencyWarning(Error.GetCode(), Error.GetDiagnostic(), TEXT("preview_verify"));
				Error = HjsonWorkspace::FWorkspaceError();
				continue;
			}
			UeAgentAuthoringObjectApply::FApplyResult Result;
			if (!UeAgentAuthoringObjectApply::VerifyPrepared(Plan.Prepared, Result))
			{
				SetPreviewFailure(Error, Result.Diagnostic, Plan.SourceUnit, TEXT("preview_verify"), Plan.Target, Plan.Descriptors, LocateFailure);
				UeAgentAuthoringDiagnostics::ReportConsistencyWarning(Error.GetCode(), Error.GetDiagnostic(), TEXT("preview_verify"));
				Error = HjsonWorkspace::FWorkspaceError();
			}
		}
		return true;
	}

	bool FPreviewWorkspaceApply::Rollback(HjsonWorkspace::FWorkspaceError& Error)
	{
		bool bRestored = true;
		for (int32 Index = Locals.Num() - 1; Index >= 0; --Index)
		{
			auto& Plan = Locals[Index];
			if (!Plan.Prepared.HasBeenApplied()) continue;
			if (!Plan.PreparedOwner.IsValid())
			{
				SetPreviewFailure(Error, MissingPreviewOwner(TEXT("preview_local_restore_owner_expired"),
					TEXT("The prepared local preview owner expired before rollback"), TEXT("preview_rollback"), TEXT("prepared_owner_present")),
					Plan.SourceUnit, TEXT("preview_rollback"), Plan.State->AssetPath, Plan.Descriptors, LocateFailure, true);
				bRestored = false; continue;
			}
			UeAgentAuthoringObjectApply::FApplyResult Result;
			if (!UeAgentAuthoringObjectApply::RollbackPrepared(Plan.Prepared, Result))
			{
				SetPreviewFailure(Error, Result.Diagnostic, Plan.SourceUnit, TEXT("preview_rollback"), Plan.State->AssetPath, Plan.Descriptors, LocateFailure, true);
				bRestored = false; continue;
			}
			UE::UAI::Contract::FDiagnostic NativeError;
			if (!AssetPreview::ApplyLocalState(*Plan.State, Plan.Group, Plan.Fields, NativeError))
			{
				SetPreviewFailure(Error, NativeError, Plan.SourceUnit, TEXT("preview_rollback"), Plan.State->AssetPath, Plan.Descriptors, LocateFailure, true);
				bRestored = false;
			}
		}
		for (int32 Index = Profiles.Num() - 1; Index >= 0; --Index)
		{
			auto& Plan = Profiles[Index];
			if (!Plan.Prepared.HasBeenApplied()) continue;
			if (!ValidateOwner(Plan, Error, TEXT("preview_rollback"), true))
			{
				bRestored = false;
				continue;
			}
			UeAgentAuthoringObjectApply::FApplyResult Result;
			if (!UeAgentAuthoringObjectApply::RollbackPrepared(Plan.Prepared, Result))
			{
				SetPreviewFailure(Error, Result.Diagnostic, Plan.SourceUnit, TEXT("preview_rollback"), Plan.Target, Plan.Descriptors, LocateFailure, true);
				bRestored = false;
			}
			if (!Notify(Plan, Error, TEXT("preview_rollback"), true)) bRestored = false;
		}
		return bRestored;
	}
}

#include "DomainAdapters/LocalizationStringTable/UeAgentLocalizationSettingsAuthoringApply.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "DomainAdapters/LocalizationStringTable/UeAgentLocalizationSettingsAuthoringState.h"
#include "DomainAdapters/LocalizationStringTable/UeAgentLocalizationSettingsConfig.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Internationalization/Internationalization.h"
#include "Internationalization/Culture.h"
#include "LocalizationConfigurationScript.h"
#include "LocalizationSettings.h"
#include "Misc/Paths.h"
#include "UObject/Class.h"
#include "UObject/StrongObjectPtr.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceDependencyGraph.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceLoader.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"
#include "Utilities/AuthoringObjectApply/UeAgentAuthoringObjectApply.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

namespace UE::UAI::DomainAdapters::LocalizationStringTable
{
	namespace
	{
		using namespace UeAgentAuthoringWorkspaceTransaction;
		UeAgentApplyWorkspaceContract::FContract WorkspaceContract()
		{
			using namespace UeAgentApplyWorkspaceContract;
			FContract Contract;
			Contract.Schema = TEXT("uai.apply_workspace_contract.v1");
			Contract.ContractId = TEXT("uai.apply_workspace.localization_settings");
			Contract.ContractVersion = TEXT("1.0.0");
			Contract.ProfileId = TEXT("localization_settings");
			Contract.Sources = {
				{ESourceKind::Directory, TEXT("physical"), TEXT("object")},
				{ESourceKind::SingleFile, TEXT("virtual"), TEXT("object")},
				{ESourceKind::Inline, TEXT("virtual"), TEXT("object")}};
			Contract.Entry.DirectoryEntry = TEXT("index.hjson");
			Contract.Entry.VirtualUnitId = TEXT("settings");
			FUnitDescriptor Unit;
			Unit.UnitId = TEXT("settings");
			Unit.PathPattern = TEXT("index.hjson");
			Unit.Role = TEXT("object");
			Unit.Cardinality = TEXT("one");
			Unit.bRequired = true;
			Unit.RouteId = TEXT("localization_settings");
			Unit.ApplyContractRef = TEXT("uai.apply_input.localization_settings.native");
			Unit.MergePolicy = TEXT("replace");
			Unit.AbsentSemantics = TEXT("error");
			Unit.EmptySemantics = TEXT("error");
			Contract.Units = {Unit};
			Contract.ReferencePolicy.Base = TEXT("workspace_root");
			Contract.ReferencePolicy.RootContainment = TEXT("required");
			Contract.ReferencePolicy.ExternalReferences = TEXT("reject");
			Contract.ReferencePolicy.CaseCollision = TEXT("reject");
			Contract.DependencyPolicy.EdgeSource = TEXT("contract_and_references");
			Contract.DependencyPolicy.CyclePolicy = TEXT("reject");
			Contract.DependencyPolicy.UnresolvedPolicy = TEXT("reject");
			Contract.DependencyPolicy.Order = TEXT("stable_topological");
			Contract.Preflight.Scope = TEXT("whole_workspace");
			Contract.Preflight.MutationBeforeSuccess = TEXT("forbid");
			Contract.Preflight.bDiagnosticsIncludeSource = true;
			Contract.Transaction.Mode = TEXT("atomic");
			Contract.Transaction.RollbackOn = {TEXT("mutation_failed"), TEXT("compile_failed"), TEXT("save_failed"), TEXT("verification_failed")};
			Contract.Execution.Compile = TEXT("provider_hook");
			Contract.Execution.Save = TEXT("caller_controlled");
			Contract.Execution.Verification = TEXT("shared_snapshot_required");
			Contract.UnknownFilePolicy = TEXT("reject");
			Contract.DuplicateTargetPolicy = TEXT("reject");
			return Contract;
		}
		bool Fail(FOperationFailure& Failure, const UE::UAI::Contract::FDiagnostic& Diagnostic)
		{ Failure.Diagnostic = Diagnostic; return false; }
		UE::UAI::Contract::FDiagnostic LocalizationFailure(const FString& Code, const FString& Reason,
			const FString& Field, const TSharedPtr<FJsonValue>& Actual, const TSharedPtr<FJsonValue>& Expected)
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("authoring_apply"));
			Error.SourceKind = TEXT("native_result");
			Error.TargetPath = GetDefault<ULocalizationSettings>()->GetPathName();
			Error.PropertyPath = Field;
			Error.ActualValue = Actual;
			Error.ExpectedValue = Expected;
			Error.ActualValuePresent = Actual.IsValid();
			Error.ExpectedValuePresent = Expected.IsValid();
			return Error;
		}

		UE::UAI::Contract::FDiagnostic LocalizationCondition(const FString& Code, const FString& Reason,
			const FString& Field, bool Actual, bool Expected = true)
		{
			return LocalizationFailure(Code, Reason, Field, MakeShared<FJsonValueBoolean>(Actual), MakeShared<FJsonValueBoolean>(Expected));
		}

		TSharedPtr<FJsonValue> InputFact(const TSharedPtr<FJsonValue>& Value)
		{
			if (!Value.IsValid()) return nullptr;
			if (Value->Type == EJson::Object || Value->Type == EJson::Array)
				return MakeShared<FJsonValueString>(Value->Type == EJson::Array ? TEXT("array") : TEXT("object"));
			return Value;
		}

		UE::UAI::Contract::FDiagnostic LocalizationInputFailure(const FJsonObject* Root,
			const FString& Code, const FString& Reason, const FString& Field,
			const TSharedPtr<FJsonValue>& Actual, const TSharedPtr<FJsonValue>& Expected)
		{
			auto Error = LocalizationFailure(Code, Reason, Field, Actual, Expected);
			Error.SourceKind = Actual.IsValid() ? TEXT("authored_value") : TEXT("related_input");
			Error.Source.JsonPath = Field;
			UE::UAI::Contract::FSourceLocation Source;
			if (Root && UeAgentAuthoringDiagnostics::FScopedSourceLocations::Locate(*Root, FString(), Source, Field))
				Error.Source = MoveTemp(Source);
			return Error;
		}
		struct FPrepared final : IPreparedMutation
		{
			TArray<FLocalizationTargetSettings> Before;
			TArray<FLocalizationTargetSettings> Proposed;
			TArray<FLocalizationTargetSettings> BeforeBacking;
			TArray<int32> Changed;
			TArray<TStrongObjectPtr<ULocalizationTarget>> BeforeObjects;
			TArray<TArray<FString>> ScriptPaths;
			TArray<int32> Removed;
			TArray<FDurableArtifact> RemovalArtifacts;
			TMap<FString, UeAgentAuthoringObjectApply::FPropertyIntent> SettingsIntents;
			bool VerifySettings(TArray<FLocalizationTargetSettings>& Actual, FOperationFailure& Failure) const
			{
				for (const auto& Intent : SettingsIntents)
				{
					auto* Target = Actual.FindByPredicate([&](const auto& Item) { return Item.Name == Intent.Key; });
					UeAgentAuthoringObjectApply::FApplyResult Result;
					if (!Target)
					{
						auto Error = LocalizationCondition(TEXT("localization_target_missing"),
							TEXT("The localization target required for property verification is unavailable."), TEXT("/targets/name"), false);
						Error.CandidateName = Intent.Key;
						return Fail(Failure, Error);
					}
					if (!Intent.Value.VerifyStruct(FLocalizationTargetSettings::StaticStruct(), Target, nullptr, Result))
						return Fail(Failure, Result.Diagnostic);
				}
				return true;
			}
			FString GetUnitKey() const override { return TEXT("settings"); }
			FString GetTargetKey() const override { return GetDefault<ULocalizationSettings>()->GetPathName(); }
			bool IsNoOp() const override { return Changed.IsEmpty() && Removed.IsEmpty(); }
		};
		class FPreflight final : public IPreflightProvider
		{
		public:
			bool PreflightUnit(const UeAgentAuthoringWorkspaceRouter::FWorkspaceUnit& Unit,
				TUniquePtr<IPreparedMutation>& Out, FOperationFailure& Failure) const override
			{
				const TArray<TSharedPtr<FJsonValue>>* Records = nullptr;
				if (!Unit.Document.IsValid() || !Unit.Document->TryGetArrayField(TEXT("targets"), Records))
					return Fail(Failure, LocalizationInputFailure(Unit.Document.Get(), TEXT("localization_targets_invalid"),
						TEXT("The localization document must contain a targets array."), TEXT("/targets"),
						Unit.Document ? InputFact(Unit.Document->TryGetField(TEXT("targets"))) : nullptr, MakeShared<FJsonValueString>(TEXT("array"))));
				auto Prepared = MakeUnique<FPrepared>();
				Prepared->Before = CaptureLocalizationGameSettings();
				Prepared->Proposed = Prepared->Before;
				for (const auto& Target : ULocalizationSettings::GetGameTargetSet()->TargetObjects)
				{
					if (!Target) return Fail(Failure, LocalizationCondition(TEXT("localization_target_missing"),
						TEXT("The native localization target set contains an unavailable target."),
						TEXT("/targets/") + FString::FromInt(Prepared->BeforeObjects.Num()) + TEXT("/object_present"), false));
					Prepared->BeforeObjects.Emplace(Target.Get());
				}
				UE::UAI::Contract::FDiagnostic Error;
				if (!GetLocalizationBackingSettings(*GetDefault<ULocalizationSettings>(), Prepared->BeforeBacking, Error)) return Fail(Failure, Error);
				TSet<FString> Seen;
				for (int32 RecordIndex = 0; RecordIndex < Records->Num(); ++RecordIndex)
				{
					const auto& Record = (*Records)[RecordIndex];
					const FString RecordPath = TEXT("/targets/") + FString::FromInt(RecordIndex);
					auto InputFailure = [&](const FString& Code, const FString& Reason, const FString& Field,
						const TSharedPtr<FJsonValue>& Actual, const TSharedPtr<FJsonValue>& Expected)
					{
						return Fail(Failure, LocalizationInputFailure(Unit.Document.Get(), Code, Reason, RecordPath + Field, Actual, Expected));
					};
					if (!Record || Record->Type != EJson::Object) return InputFailure(TEXT("localization_target_invalid"),
						TEXT("Each localization target must be an object."), FString(), InputFact(Record), MakeShared<FJsonValueString>(TEXT("object")));
					const auto Item = MakeShared<FJsonObject>(*Record->AsObject());
					FString Name, Native;
					const TArray<TSharedPtr<FJsonValue>>* Cultures = nullptr;
					if (!Item->HasTypedField<EJson::String>(TEXT("name")) || !Item->TryGetStringField(TEXT("name"), Name))
						return InputFailure(TEXT("localization_target_invalid"), TEXT("The localization target name must be a string."),
							TEXT("/name"), InputFact(Item->TryGetField(TEXT("name"))), MakeShared<FJsonValueString>(TEXT("string")));
					if (Name.IsEmpty() || Seen.Contains(Name) || Name.Contains(TEXT("/")) || Name.Contains(TEXT("\\"))
						|| Name.Contains(TEXT("..")) || Name.Contains(TEXT(":")))
						return InputFailure(TEXT("localization_target_invalid"), Name.IsEmpty() ? TEXT("The localization target name must not be empty.")
							: Seen.Contains(Name) ? TEXT("The localization target name duplicates an earlier target.")
							: TEXT("The localization target name contains a forbidden path separator, traversal sequence or colon."),
							TEXT("/name"), MakeShared<FJsonValueString>(Name), MakeShared<FJsonValueString>(TEXT("nonempty unique name without /, backslash, .. or colon")));
					Seen.Add(Name);
					int32 Index = Prepared->Before.IndexOfByPredicate([&](const auto& Value) { return Value.Name.Equals(Name, ESearchCase::CaseSensitive); });
					const bool bNew = Index == INDEX_NONE;
					if (bNew)
					{
						FText PathError;
						if (!FPaths::ValidatePath(Name, &PathError) || Name.TrimStartAndEnd() != Name
							|| Name.EndsWith(TEXT(".")) || Prepared->Before.ContainsByPredicate([&](const auto& Value) { return Value.Name.Equals(Name, ESearchCase::IgnoreCase); }))
							return InputFailure(TEXT("localization_target_name_invalid"),
							TEXT("The new localization target name is not a unique valid path segment: ") + PathError.ToString(),
							TEXT("/name"), MakeShared<FJsonValueString>(Name),
							MakeShared<FJsonValueString>(TEXT("valid path segment without surrounding whitespace, trailing dot or case-insensitive collision")));
						FLocalizationTargetSettings Added;
						Added.Name = Name;
						Added.NativeCultureIndex = Added.SupportedCulturesStatistics.Add(FCultureStatistics(FInternationalization::Get().GetCurrentCulture()->GetName()));
						Index = Prepared->Proposed.Add(MoveTemp(Added));
						if (!Item->HasField(TEXT("native_culture"))) Item->SetStringField(TEXT("native_culture"), Prepared->Proposed[Index].SupportedCulturesStatistics[0].CultureName);
						if (!Item->HasField(TEXT("cultures"))) Item->SetArrayField(TEXT("cultures"), {MakeShared<FJsonValueString>(Prepared->Proposed[Index].SupportedCulturesStatistics[0].CultureName)});
					}
					if (const auto Settings = Item->TryGetField(TEXT("settings")))
					{
						using namespace UeAgentAuthoringObjectApply;
						auto& Target = Prepared->Proposed[Index];
						UeAgentAuthoringState::FStateSnapshot Snapshot;
						TSharedPtr<FJsonObject> Properties;
						TArray<TSharedPtr<FJsonValue>> Descriptors;
						if (!CaptureLocalizationTargetProperties(Target, Snapshot, Properties, Error)
							|| !UeAgentDetailsAuthoringService::DecodeReadableProperties(Snapshot, Settings, Descriptors, Error))
						{
							UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error,
								TEXT("/targets/") + FString::FromInt(RecordIndex) + TEXT("/settings"));
							return Fail(Failure, Error);
						}
						FOptions Options;
						Options.bEmitEditorNotifications = false;
						Options.bMarkPackageDirty = false;
						Options.bCompactValues = true;
						Options.IncludeRootProperty = IsLocalizationSettingProperty;
						FPreparedApply Plan;
						FFailure PropertyFailure;
						FApplyResult Result;
						auto SettingsFailure = [&](UE::UAI::Contract::FDiagnostic Diagnostic) {
							UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Diagnostic, RecordPath + TEXT("/settings"), true);
							return Fail(Failure, Diagnostic);
						};
						if (!PrepareStruct(FLocalizationTargetSettings::StaticStruct(), &Target, nullptr, Descriptors, Options, Plan, PropertyFailure))
							return SettingsFailure(PropertyFailure.Diagnostic);
						if (!ApplyPrepared(Plan, Result)) return SettingsFailure(Result.Diagnostic);
						if (!Plan.CaptureIntent(Prepared->SettingsIntents.FindOrAdd(Name), PropertyFailure))
							return SettingsFailure(PropertyFailure.Diagnostic);
					}
					const auto& Before = Prepared->Proposed[Index];
					const bool bHasNative = Before.SupportedCulturesStatistics.IsValidIndex(Before.NativeCultureIndex);
					bool bUnchanged = bHasNative
						? Item->HasTypedField<EJson::String>(TEXT("native_culture"))
							&& Item->GetStringField(TEXT("native_culture")).Equals(Before.SupportedCulturesStatistics[Before.NativeCultureIndex].CultureName, ESearchCase::CaseSensitive)
						: !Item->HasField(TEXT("native_culture"));
					bUnchanged = bUnchanged && Item->TryGetArrayField(TEXT("cultures"), Cultures)
						&& Cultures->Num() == Before.SupportedCulturesStatistics.Num();
					if (bUnchanged)
					{
						for (int32 CultureIndex = 0; CultureIndex < Cultures->Num(); ++CultureIndex)
						{
							const auto& Value = (*Cultures)[CultureIndex];
							if (!Value || Value->Type != EJson::String || !Value->AsString().Equals(
								Before.SupportedCulturesStatistics[CultureIndex].CultureName, ESearchCase::CaseSensitive))
							{ bUnchanged = false; break; }
						}
					}
					if (bUnchanged && !bNew) continue;
					if (!Item->HasTypedField<EJson::String>(TEXT("native_culture")) || !Item->TryGetStringField(TEXT("native_culture"), Native))
						return InputFailure(TEXT("localization_target_state_incomplete"), TEXT("A changed localization target must declare its native_culture string."),
							TEXT("/native_culture"), InputFact(Item->TryGetField(TEXT("native_culture"))), MakeShared<FJsonValueString>(TEXT("string")));
					if (!Item->TryGetArrayField(TEXT("cultures"), Cultures))
						return InputFailure(TEXT("localization_target_state_incomplete"), TEXT("A changed localization target must declare its cultures array."),
							TEXT("/cultures"), InputFact(Item->TryGetField(TEXT("cultures"))), MakeShared<FJsonValueString>(TEXT("array")));
					auto& Proposed = Prepared->Proposed[Index];
					const auto BeforeCultures = Proposed.SupportedCulturesStatistics;
					Proposed.SupportedCulturesStatistics.Reset();
					Proposed.NativeCultureIndex = INDEX_NONE;
					TSet<FString> CultureNames;
					for (int32 CultureIndex = 0; CultureIndex < Cultures->Num(); ++CultureIndex)
					{
						const auto& CultureValue = (*Cultures)[CultureIndex];
						const FString CulturePath = TEXT("/cultures/") + FString::FromInt(CultureIndex);
						if (!CultureValue || CultureValue->Type != EJson::String) return InputFailure(TEXT("localization_target_cultures_invalid"),
							TEXT("Each localization culture must be a string."), CulturePath, InputFact(CultureValue), MakeShared<FJsonValueString>(TEXT("culture name string")));
						const FString Culture = CultureValue->AsString();
						if (CultureNames.Contains(Culture) || !FInternationalization::Get().GetCulture(Culture).IsValid())
							return InputFailure(TEXT("localization_target_cultures_invalid"),
								CultureNames.Contains(Culture) ? TEXT("The culture duplicates an earlier culture in this target.") : TEXT("Unreal does not recognize the culture name."),
								CulturePath, MakeShared<FJsonValueString>(Culture), MakeShared<FJsonValueString>(TEXT("unique culture recognized by Unreal")));
						CultureNames.Add(Culture);
						const auto* Existing = BeforeCultures.FindByPredicate([&](const auto& Value) { return Value.CultureName == Culture; });
						FCultureStatistics Value = Existing ? *Existing : FCultureStatistics();
						Value.CultureName = Culture;
						if (Culture.Equals(Native, ESearchCase::CaseSensitive)) Proposed.NativeCultureIndex = Proposed.SupportedCulturesStatistics.Num();
						Proposed.SupportedCulturesStatistics.Add(Value);
					}
					if (Proposed.NativeCultureIndex == INDEX_NONE) return InputFailure(TEXT("localization_target_native_culture_missing"),
						TEXT("The declared native culture is absent from the target cultures array."), TEXT("/native_culture"),
						MakeShared<FJsonValueString>(Native), MakeShared<FJsonValueString>(TEXT("a member of this target's cultures")));
					if (bNew || !FLocalizationTargetSettings::StaticStruct()->CompareScriptStruct(&Proposed, &Prepared->Before[Index], PPF_None)) Prepared->Changed.Add(Index);
				}
				for (int32 Index = 0; Index < Prepared->Before.Num(); ++Index)
				{
					const auto& Before = Prepared->Before[Index];
					if (Seen.Contains(Before.Name)) continue;
					if (Before.Name.IsEmpty() || Before.Name != Before.Name.TrimStartAndEnd()
						|| !FPaths::ValidatePath(Before.Name) || Before.Name.Contains(TEXT("/")) || Before.Name.Contains(TEXT("\\"))
						|| Before.Name.Contains(TEXT("..")) || Before.Name.Contains(TEXT(":")) || Before.Name.EndsWith(TEXT(".")))
						return Fail(Failure, LocalizationFailure(TEXT("localization_target_name_invalid"),
							TEXT("The existing localization target name is unsafe for removing its native artifacts."),
							TEXT("/targets/") + FString::FromInt(Index) + TEXT("/name"), MakeShared<FJsonValueString>(Before.Name),
							MakeShared<FJsonValueString>(TEXT("nonempty valid path segment without traversal, surrounding whitespace, separator, colon or trailing dot"))));
					Prepared->Removed.Add(Index);
					const auto* Target = Prepared->BeforeObjects[Index].Get();
					for (const auto& Path : LocalizationConfigurationScript::GetConfigPaths(Target))
						Prepared->RemovalArtifacts.Add({TEXT("localization.removed.") + FString::FromInt(Prepared->RemovalArtifacts.Num()), FPaths::ConvertRelativePathToFull(Path)});
					FDurableArtifact Data;
					Data.ArtifactId = TEXT("localization.removed.") + FString::FromInt(Prepared->RemovalArtifacts.Num());
					Data.FinalPath = FPaths::ConvertRelativePathToFull(LocalizationConfigurationScript::GetDataDirectory(Target));
					Data.Kind = EDurableArtifactKind::DirectoryTree;
					Prepared->RemovalArtifacts.Add(MoveTemp(Data));
				}
				Prepared->Proposed.RemoveAll([&](const auto& Value) { return !Seen.Contains(Value.Name); });
				Prepared->Changed.Reset();
				for (int32 Index = 0; Index < Prepared->Proposed.Num(); ++Index)
				{
					const auto& Value = Prepared->Proposed[Index];
					const auto* Before = Prepared->Before.FindByPredicate([&](const auto& Item) { return Item.Name.Equals(Value.Name, ESearchCase::CaseSensitive); });
					if (!Before || !FLocalizationTargetSettings::StaticStruct()->CompareScriptStruct(&Value, Before, PPF_None)) Prepared->Changed.Add(Index);
				}
				Prepared->ScriptPaths.SetNum(Prepared->Proposed.Num());
				TStrongObjectPtr<ULocalizationSettings> ShadowSettings(NewObject<ULocalizationSettings>(GetTransientPackage()));
				TStrongObjectPtr<ULocalizationTargetSet> ShadowSet(NewObject<ULocalizationTargetSet>(ShadowSettings.Get()));
				for (int32 Index : Prepared->Changed)
				{
					TStrongObjectPtr<ULocalizationTarget> ShadowTarget(NewObject<ULocalizationTarget>(ShadowSet.Get()));
					ShadowTarget->Settings = Prepared->Proposed[Index];
					Prepared->ScriptPaths[Index] = LocalizationConfigurationScript::GetConfigPaths(ShadowTarget.Get());
					if (!Prepared->Before.ContainsByPredicate([&](const auto& Value) { return Value.Name.Equals(ShadowTarget->Settings.Name, ESearchCase::CaseSensitive); }))
					{
						const FString DataPath = LocalizationConfigurationScript::GetDataDirectory(ShadowTarget.Get());
						if (FPaths::DirectoryExists(DataPath) || FPaths::FileExists(DataPath))
							return Fail(Failure, LocalizationFailure(TEXT("localization_target_data_conflict"),
								TEXT("The new localization target's data path already exists."), TEXT("/data_path"),
								MakeShared<FJsonValueString>(DataPath), MakeShared<FJsonValueString>(TEXT("unused path"))));
						for (const auto& Path : Prepared->ScriptPaths[Index])
							if (FPaths::FileExists(Path) || FPaths::DirectoryExists(Path)) return Fail(Failure, LocalizationFailure(TEXT("localization_target_script_conflict"),
								TEXT("The new localization target's script path already exists."), TEXT("/script_path"),
								MakeShared<FJsonValueString>(Path), MakeShared<FJsonValueString>(TEXT("unused path"))));
					}
				}
				Out = MoveTemp(Prepared);
				return true;
			}
		};

		class FBackend final : public ITransactionBackend
		{
		public:
			FBackend(FPrepared& InState, TFunction<bool(FOperationFailure&)> InFinalReadback, FLocalizationSettingsStage InStage)
				: State(&InState), FinalReadback(MoveTemp(InFinalReadback)), Stage(MoveTemp(InStage)) {}
			bool BeginWorkspace(const UeAgentAuthoringWorkspaceRouter::FWorkspace&,
				const UeAgentAuthoringWorkspaceDependencyGraph::FDependencyGraph&, FOperationFailure&) override { return true; }
			bool CaptureRollback(IPreparedMutation& Mutation, FOperationFailure& Failure) override
			{
				State = &static_cast<FPrepared&>(Mutation);
				UE::UAI::Contract::FDiagnostic Error;
				return EqualLocalizationNativeValues(CaptureLocalizationGameSettings(), State->Before, Error)
					|| Fail(Failure, UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("localization_preflight_state_changed"), Error));
			}
			bool ApplyUnit(IPreparedMutation& Mutation, FOperationFailure& Failure) override
			{
				State = &static_cast<FPrepared&>(Mutation);
				auto* Set = ULocalizationSettings::GetGameTargetSet();
				UE::UAI::Contract::FDiagnostic Error;
				if (!CheckOriginalObjects(Error)) return Fail(Failure, Error);
				TArray<TObjectPtr<ULocalizationTarget>> DesiredObjects;
				for (const auto& Proposed : State->Proposed)
				{
					const int32 BeforeIndex = State->Before.IndexOfByPredicate([&](const auto& Value) { return Value.Name.Equals(Proposed.Name, ESearchCase::CaseSensitive); });
					if (BeforeIndex != INDEX_NONE) DesiredObjects.Add(State->BeforeObjects[BeforeIndex].Get());
					else
					{
						auto* Added = NewObject<ULocalizationTarget>(Set);
						Added->Settings = Proposed;
						DesiredObjects.Add(Added);
					}
				}
				Set->TargetObjects = MoveTemp(DesiredObjects);
				return Assign(State->Proposed, State->Proposed, Failure);
			}
			bool Compile(FOperationFailure&) override { return true; }
			bool Save(FOperationFailure&) override { return true; }
			bool Verify(FOperationFailure& Failure) override
			{
				TArray<FLocalizationTargetSettings> Backing;
				UE::UAI::Contract::FDiagnostic Error;
				auto Actual = CaptureLocalizationGameSettings();
				if (!State) return Fail(Failure, LocalizationCondition(TEXT("localization_verification_state_missing"),
					TEXT("The prepared localization state required for verification is unavailable."), TEXT("/prepared_state_present"), false));
				if (!State->VerifySettings(Actual, Failure)) return false;
				if (!EqualLocalizationNativeValues(Actual, State->Proposed, Error)) return Fail(Failure, Error);
				if (!GetLocalizationBackingSettings(*GetDefault<ULocalizationSettings>(), Backing, Error)) return Fail(Failure, Error);
				return EqualLocalizationNativeValues(Backing, State->Proposed, Error) || Fail(Failure, Error);
			}
			bool RollbackUnit(IPreparedMutation& Mutation, FOperationFailure& Failure) override
			{
				State = &static_cast<FPrepared&>(Mutation);
				auto* Set = ULocalizationSettings::GetGameTargetSet();
				if (!Set) return Fail(Failure, LocalizationCondition(TEXT("localization_target_set_missing"),
					TEXT("The native localization target set required for rollback is unavailable."), TEXT("/target_set_present"), false));
				Set->TargetObjects.Reset();
				for (const auto& Original : State->BeforeObjects) Set->TargetObjects.Add(Original.Get());
				// As in the native dashboard, removing the target-set reference ends
				// ownership. UE manages collection; startup targets can be permanent GC roots.
				return Assign(State->Before, State->BeforeBacking, Failure);
			}
			bool EndWorkspace(bool, FOperationFailure&) override { return true; }
			bool SupportsDurableTransactions() const override { return true; }
			bool RequiresPersistentReadback() const override { return true; }
			bool RequiresMemoryCheckpoint() const override { return false; }
			bool VerifyRestoredState(FOperationFailure& Failure) override
			{
				TArray<FLocalizationTargetSettings> Backing;
				UE::UAI::Contract::FDiagnostic Error;
				if (!CheckOriginalObjects(Error)) return Fail(Failure, Error);
				if (!EqualLocalizationNativeValues(CaptureLocalizationGameSettings(), State->Before, Error)) return Fail(Failure, Error);
				if (!GetLocalizationBackingSettings(*GetDefault<ULocalizationSettings>(), Backing, Error)) return Fail(Failure, Error);
				return EqualLocalizationNativeValues(Backing, State->BeforeBacking, Error) || Fail(Failure, Error);
			}
			bool BuildDurableArtifactInventory(const TArray<IPreparedMutation*>& Mutations,
				TArray<FDurableArtifact>& Artifacts, FOperationFailure& Failure) override
			{
				if (Mutations.Num() != 1) return Fail(Failure, LocalizationFailure(TEXT("localization_mutation_count_invalid"),
					TEXT("Exactly one localization mutation is required to build the durable artifact inventory."),
					TEXT("/mutations/count"), MakeShared<FJsonValueNumber>(Mutations.Num()), MakeShared<FJsonValueNumber>(1)));
				State = &static_cast<FPrepared&>(*Mutations[0]);
				Artifacts.Reset();
				Artifacts.Add({TEXT("localization.settings"), FPaths::ConvertRelativePathToFull(GetDefault<ULocalizationSettings>()->GetDefaultConfigFilename())});
				for (int32 Index : State->Changed)
				{
					for (const FString& Path : State->ScriptPaths[Index])
						Artifacts.Add({TEXT("localization.script.") + FString::FromInt(Artifacts.Num()), FPaths::ConvertRelativePathToFull(Path)});
				}
				Artifacts.Append(State->RemovalArtifacts);
				return true;
			}
			bool StageDurableArtifacts(TArray<FDurableArtifact>& Artifacts, FOperationFailure& Failure) override
			{
				auto NativeStage = [this, &Artifacts](FOperationFailure& StageFailure) { return StageNativeArtifacts(Artifacts, StageFailure); };
				return Stage ? Stage(NativeStage, Failure) : NativeStage(Failure);
			}
			bool StageNativeArtifacts(TArray<FDurableArtifact>& Artifacts, FOperationFailure& Failure)
			{
				if (!State) return Fail(Failure, LocalizationCondition(TEXT("localization_stage_state_missing"),
					TEXT("The prepared localization state required for staging is unavailable."), TEXT("/prepared_state_present"), false));
				if (Artifacts.IsEmpty()) return Fail(Failure, LocalizationFailure(TEXT("localization_stage_state_missing"),
					TEXT("The localization artifact inventory must contain the settings config before staging."),
					TEXT("/artifacts/count"), MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("at least 1"))));
				UE::UAI::Contract::FDiagnostic Error;
				if (!WriteLocalizationSettingsStage(State->Proposed, Artifacts[0].StagingPath, Error)) return Fail(Failure, Error);
				Artifacts[0].bStagedExists = true;
				int32 ArtifactIndex = 1;
				for (int32 Index : State->Changed)
				{
					const auto* Target = ULocalizationSettings::GetGameTargetSet()->TargetObjects[Index].Get();
					using namespace LocalizationConfigurationScript;
					TArray<FLocalizationConfigurationScript> Scripts{
						GenerateGatherTextConfigFile(Target), GenerateImportTextConfigFile(Target), GenerateExportTextConfigFile(Target),
						GenerateImportDialogueScriptConfigFile(Target), GenerateExportDialogueScriptConfigFile(Target), GenerateImportDialogueConfigFile(Target),
						GenerateCompileTextConfigFile(Target), GenerateWordCountReportConfigFile(Target)};
					for (auto& Script : Scripts)
					{
						if (!Artifacts.IsValidIndex(ArtifactIndex))
							return Fail(Failure, LocalizationFailure(TEXT("localization_script_stage_failed"),
								TEXT("The generated localization script has no corresponding durable artifact."),
								TEXT("/artifacts/") + FString::FromInt(ArtifactIndex), MakeShared<FJsonValueNumber>(Artifacts.Num()),
								MakeShared<FJsonValueString>(TEXT("artifact index within the inventory"))));
						if (!Script.Write(Artifacts[ArtifactIndex].StagingPath, false))
						{
							auto ScriptError = LocalizationCondition(TEXT("localization_script_stage_failed"),
								TEXT("Unreal could not write the generated localization script to its staging file."),
								TEXT("/artifacts/") + FString::FromInt(ArtifactIndex) + TEXT("/write_succeeded"), false);
							ScriptError.Source.SourceFile = Artifacts[ArtifactIndex].StagingPath;
							ScriptError.CandidateName = Target->Settings.Name;
							return Fail(Failure, ScriptError);
						}
						Artifacts[ArtifactIndex++].bStagedExists = true;
					}
				}
				return ArtifactIndex + State->RemovalArtifacts.Num() == Artifacts.Num()
					|| Fail(Failure, LocalizationFailure(TEXT("localization_artifact_inventory_mismatch"),
						TEXT("The staged script and removal counts do not match the durable artifact inventory."),
						TEXT("/artifacts/count"), MakeShared<FJsonValueNumber>(Artifacts.Num()),
						MakeShared<FJsonValueNumber>(ArtifactIndex + State->RemovalArtifacts.Num())));
			}
			bool FinalizeVerification(FOperationFailure& Failure) override
			{
				TArray<FLocalizationTargetSettings> Readback;
				UE::UAI::Contract::FDiagnostic Error;
				if (!State) return Fail(Failure, LocalizationCondition(TEXT("localization_verification_state_missing"),
					TEXT("The prepared localization state required for persistent readback is unavailable."), TEXT("/prepared_state_present"), false));
				const FString ConfigFile = GetDefault<ULocalizationSettings>()->GetDefaultConfigFilename();
				if (!ReadLocalizationSettingsFile(ConfigFile, Readback, Error)) return Fail(Failure, Error);
				if (!EqualLocalizationConfigValues(Readback, State->Proposed, Error))
				{
					Error.Source.SourceFile = ConfigFile;
					UeAgentAuthoringDiagnostics::ReportConsistencyWarning(TEXT("localization_persistent_verification_failed"), Error, TEXT("localization_observation"));
				}
				if (!State->VerifySettings(Readback, Failure)) {
					UeAgentAuthoringDiagnostics::ReportConsistencyWarning(Failure.Diagnostic.Code, Failure.Diagnostic, TEXT("localization_observation"));
					Failure = FOperationFailure();
				}
				return !FinalReadback || FinalReadback(Failure);
			}
		private:
			bool CheckOriginalObjects(UE::UAI::Contract::FDiagnostic& Error) const
			{
				const auto* Set = ULocalizationSettings::GetGameTargetSet();
				if (!State || !Set)
				{
					Error = LocalizationCondition(TEXT("localization_target_set_changed"),
						!State ? TEXT("The prepared localization checkpoint is unavailable.") : TEXT("The native localization target set is unavailable."),
						!State ? TEXT("/prepared_state_present") : TEXT("/target_set_present"), false);
					return false;
				}
				if (Set->TargetObjects.Num() != State->BeforeObjects.Num())
				{
					Error = LocalizationFailure(TEXT("localization_target_set_changed"),
						TEXT("The native localization target object count changed after preflight."), TEXT("/targets/count"),
						MakeShared<FJsonValueNumber>(Set->TargetObjects.Num()), MakeShared<FJsonValueNumber>(State->BeforeObjects.Num()));
					return false;
				}
				for (int32 Index = 0; Index < State->BeforeObjects.Num(); ++Index)
					if (Set->TargetObjects[Index].Get() != State->BeforeObjects[Index].Get())
					{
						Error = LocalizationCondition(TEXT("localization_target_set_changed"),
							TEXT("The native localization target object no longer matches its preflight checkpoint."),
							TEXT("/targets/") + FString::FromInt(Index) + TEXT("/object_matches_checkpoint"), false);
						Error.ActualEvidence = MakeShared<FJsonValueString>(GetPathNameSafe(Set->TargetObjects[Index].Get()));
						Error.ExpectedEvidence = MakeShared<FJsonValueString>(GetPathNameSafe(State->BeforeObjects[Index].Get()));
						return false;
					}
				return true;
			}
			bool Assign(const TArray<FLocalizationTargetSettings>& Native, const TArray<FLocalizationTargetSettings>& Backing, FOperationFailure& Failure)
			{
				auto* Set = ULocalizationSettings::GetGameTargetSet();
				if (!Set) return Fail(Failure, LocalizationCondition(TEXT("localization_target_set_changed"),
					TEXT("The native localization target set is unavailable during state assignment."), TEXT("/target_set_present"), false));
				if (Set->TargetObjects.Num() != Native.Num()) return Fail(Failure, LocalizationFailure(TEXT("localization_target_set_changed"),
					TEXT("The native localization target count does not match the state being assigned."),
					TEXT("/targets/count"), MakeShared<FJsonValueNumber>(Set->TargetObjects.Num()), MakeShared<FJsonValueNumber>(Native.Num())));
				for (int32 Index = 0; Index < Native.Num(); ++Index)
				{
					if (!Set->TargetObjects[Index]) return Fail(Failure, LocalizationCondition(TEXT("localization_target_missing"),
						TEXT("The native localization target required for state assignment is unavailable."),
						TEXT("/targets/") + FString::FromInt(Index) + TEXT("/object_present"), false));
					Set->TargetObjects[Index]->Settings = Native[Index];
				}
				UE::UAI::Contract::FDiagnostic Error;
				return SetLocalizationBackingSettings(*GetMutableDefault<ULocalizationSettings>(), Backing, Error) || Fail(Failure, Error);
			}
			FPrepared* State = nullptr;
			TFunction<bool(FOperationFailure&)> FinalReadback;
			FLocalizationSettingsStage Stage;
		};
	}

	bool ApplyLocalizationSettings(const FString& RequestId, const TSharedRef<FJsonObject>& Desired,
		HjsonWorkspace::DomainAuthoring::FRootPropertyApplyOutcome& Outcome, const FLocalizationSettingsStage& Stage)
	{
		using namespace UeAgentAuthoringWorkspaceTransaction;
		const auto Contract = WorkspaceContract();
		UeAgentAuthoringWorkspaceLoader::FLoadResult Load;
		if (!UeAgentAuthoringWorkspaceLoader::LoadWorkspace(Contract,
			UeAgentAuthoringWorkspaceLoader::FSourceRequest::MakeInlineObject(Desired, TEXT("inline://localization-settings")), Load))
		{ Outcome.Diagnostic = Load.Diagnostic; return false; }
		UeAgentAuthoringWorkspaceDependencyGraph::FDependencyGraph Graph;
		UeAgentAuthoringWorkspaceDependencyGraph::FGraphFailure GraphFailure;
		if (!UeAgentAuthoringWorkspaceDependencyGraph::BuildDependencyGraph(Contract, Load.Workspace, {}, Graph, GraphFailure))
		{ Outcome.Diagnostic = GraphFailure.Diagnostic; return false; }
		FPreflightResult Prepared;
		if (!PreflightWorkspace(Contract, Load.Workspace, Graph, FPreflight(), Prepared))
		{ Outcome.Diagnostic = Prepared.Diagnostic; return false; }
		FExecutionOptions Options;
		FOperationFailure Failure;
		if (!UeAgentAuthoringOperationApply::BuildApplyEntryExecutionOptions(RequestId, Options, Failure))
		{ Outcome.Diagnostic = Failure.Diagnostic; return false; }
		Options.bSaveWhenCallerControlled = true;
		if (Prepared.Units.Num() != 1 || !Prepared.Units[0].Mutation)
		{
			Outcome.Diagnostic = LocalizationFailure(TEXT("localization_mutation_count_invalid"),
				Prepared.Units.Num() != 1 ? TEXT("Exactly one localization mutation must be prepared.")
					: TEXT("The prepared localization unit has no mutation."),
				Prepared.Units.Num() != 1 ? TEXT("/prepared_units/count") : TEXT("/prepared_units/0/mutation_present"),
				Prepared.Units.Num() != 1 ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(Prepared.Units.Num()))
					: TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false)),
				Prepared.Units.Num() != 1 ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(1))
					: TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(true)));
			Outcome.Diagnostic.Stage = TEXT("preflight");
			return false;
		}
		FBackend Backend(static_cast<FPrepared&>(*Prepared.Units[0].Mutation), Outcome.FinalReadback, Stage);
		FExecutionResult Result;
		const bool bApplied = ExecuteAtomic(Contract, Load.Workspace, Graph, Prepared, Backend, Options, Result);
		Outcome.bApplied = bApplied;
		Outcome.Phase = Result.Phase;
		Outcome.Diagnostic = Result.Diagnostic;
		if (!bApplied && Result.bRollbackAttempted)
		{
			Outcome.Diagnostic.RollbackAttempted = true;
			Outcome.Diagnostic.RollbackSucceeded = Result.bRollbackSucceeded;
			Outcome.Diagnostic.TransactionStatus = Result.Status;
			if (!Result.bRollbackSucceeded) Outcome.Diagnostic.bRetrySafe = false;
		}
		return bApplied;
	}
}

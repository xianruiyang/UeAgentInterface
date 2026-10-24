#include "QueryOwners/Niagara/UeAgentNiagaraModuleCandidateQuery.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Core/ContentDiscovery/UeAgentReadOnlyCandidateBatch.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Core/ContentDiscovery/UeAgentNameDiscoveryProjection.h"
#include "Core/ContentDiscovery/PaletteSearch/UeAgentPaletteSearchTypes.h"
#include "Core/ContentDiscovery/UeAgentContentDiscovery.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"
#include "Domains/Niagara/Authoring/UeAgentNiagaraStageModuleAuthoring.h"
#include "Dom/JsonValue.h"
#include "NiagaraCommon.h"
#include "NiagaraEmitter.h"
#include "NiagaraSystemEditorData.h"
#include "NiagaraSystemFactoryNew.h"
#include "ViewModels/NiagaraEmitterHandleViewModel.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraSystem.h"
#include "Internationalization/Text.h"
#include "Misc/PackageName.h"
#include "ViewModels/NiagaraSystemViewModel.h"
#include "ViewModels/Stack/INiagaraStackItemGroupAddUtilities.h"
#include "ViewModels/Stack/NiagaraStackEntry.h"
#include "ViewModels/Stack/NiagaraStackScriptItemGroup.h"
#include "ViewModels/Stack/NiagaraStackViewModel.h"
#include "UObject/UnrealType.h"

#include <openssl/sha.h>

namespace UE::UAI::QueryOwners::Niagara
{
	using namespace DomainAdapters::ContentGraphRead;
	using namespace UeAgentNiagaraStageModuleAuthoring;

	namespace
	{
		const FString CandidateKind = TEXT("niagara_module");
		const FString AssetModuleActionKind = TEXT("asset_module");

		bool ForwardDiagnostic(UE::UAI::Contract::FDiagnostic& OutError, UE::UAI::Contract::FDiagnostic Diagnostic, const FString& Subject = FString())
		{
			OutError = MoveTemp(Diagnostic);
			if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Subject;
			return false;
		}

		UE::UAI::Contract::FDiagnostic CandidateDiagnostic(FString Code, FString Subject, FString PublicReason)
		{
			auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(MoveTemp(Code), MoveTemp(PublicReason), TEXT("candidate_query"), FString(), MoveTemp(Subject));
			Diagnostic.Schema = TEXT("uai.runtime.diagnostic/1");
			return Diagnostic;
		}

		UE::UAI::Contract::FDiagnostic CandidateCause(FString Code, const FString& Subject,
			const UE::UAI::Contract::FDiagnostic& Cause, const FString& MissingReason)
		{
			auto Diagnostic = UE::UAI::Contract::FDiagnostic::WrapFailure(MoveTemp(Code), Cause, TEXT("candidate_query"),
				Cause.IsEmpty() ? MissingReason : FString());
			Diagnostic.Schema = TEXT("uai.runtime.diagnostic/1");
			if (Cause.IsEmpty()) Diagnostic.SourceKind = TEXT("native_result");
			if (Diagnostic.SubjectId.IsEmpty()) Diagnostic.SubjectId = Subject;
			return Diagnostic;
		}

		UE::UAI::Contract::FDiagnostic ContextDiagnostic(const FString& Code, const FString& Reason,
			const UObject& Target, const FNiagaraModuleCandidateContext& Context, bool bNative = true)
		{
			FString Message = Reason;
			if (!Context.TargetUsage.IsEmpty()) Message += FString::Printf(TEXT(" Usage: '%s'."), *Context.TargetUsage);
			if (!Context.EmitterName.IsEmpty()) Message += FString::Printf(TEXT(" Emitter: '%s'."), *Context.EmitterName);
			if (!Context.StageName.IsEmpty() && !Context.StageName.Equals(Context.TargetUsage, ESearchCase::IgnoreCase))
				Message += FString::Printf(TEXT(" Stage: '%s'."), *Context.StageName);
			auto Diagnostic = CandidateDiagnostic(Code, Context.StageKey, MoveTemp(Message));
			Diagnostic.TargetPath = Target.GetPathName();
			Diagnostic.SourceKind = bNative ? TEXT("native_result") : Context.SourceKind;
			if (!bNative)
			{
				Diagnostic.Source = Context.Source;
				Diagnostic.PropertyPath = Context.Source.JsonPath;
			}
			return Diagnostic;
		}

		UE::UAI::Contract::FDiagnostic UnsupportedTarget(const UObject& Target)
		{
			auto Diagnostic = ContextDiagnostic(TEXT("niagara_candidate_target_type_unsupported"),
				TEXT("Niagara candidate target must be a NiagaraSystem or NiagaraEmitter."), Target, {});
			Diagnostic.PropertyPath = TEXT("/target");
			Diagnostic.ExpectedValue = MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
				MakeShared<FJsonValueString>(TEXT("NiagaraSystem")), MakeShared<FJsonValueString>(TEXT("NiagaraEmitter"))});
			Diagnostic.ActualValue = MakeShared<FJsonValueString>(Target.GetClass()->GetName());
			Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(Target.GetClass()->GetPathName());
			return Diagnostic;
		}

		UE::UAI::Contract::FDiagnostic SelectorRequired(const FString& Name)
		{
			auto Diagnostic = CandidateDiagnostic(TEXT("niagara_candidate_selector_required"), FString(),
				TEXT("A non-empty Niagara candidate name is required."));
			Diagnostic.PropertyPath = TEXT("/name");
			Diagnostic.Source.JsonPath = TEXT("/name");
			Diagnostic.ActualValue = MakeShared<FJsonValueString>(Name);
			return Diagnostic;
		}

		FString Sha256(const FString& Text)
		{
			const FTCHARToUTF8 Utf8(*Text);
			uint8 Digest[SHA256_DIGEST_LENGTH];
			SHA256(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length(), Digest);
			FString Hex;
			Hex.Reserve(SHA256_DIGEST_LENGTH * 2);
			for (const uint8 Byte : Digest)
			{
				Hex += FString::Printf(TEXT("%02x"), Byte);
			}
			return TEXT("sha256:") + Hex;
		}

		bool IsSupportedTarget(const UObject& Target)
		{
			return Target.IsA<UNiagaraSystem>()
				|| Target.IsA<UNiagaraEmitter>();
		}

		bool ReadGuid(
			const FString& Text,
			const TCHAR* ErrorCode,
			const TCHAR* Field,
			FGuid& OutGuid,
			bool& bOutExplicit,
			FNiagaraModuleCandidateQueryError& OutError)
		{
			const FString Trimmed = Text.TrimStartAndEnd();
			bOutExplicit = !Trimmed.IsEmpty();
			OutGuid.Invalidate();
			if (!bOutExplicit)
			{
				return true;
			}
			if (!FGuid::Parse(Trimmed, OutGuid))
			{
				auto Diagnostic = CandidateDiagnostic(ErrorCode, Trimmed,
					FString::Printf(TEXT("Resolved Niagara context field '%s' is not a valid GUID."), Field));
				Diagnostic.SourceKind = TEXT("native_result");
				Diagnostic.Source.InternalJsonPath = TEXT("/") + FString(Field);
				Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("GUID"));
				Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(Text);
				return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), Trimmed);
			}
			return true;
		}

		bool ResolveContext(
			UObject& Target,
			const FNiagaraModuleCandidateContext& Requested,
			FNiagaraModuleCandidateContext& OutResolved,
			FNiagaraModuleCandidateQueryError& OutError)
		{
			OutResolved = {};
			if (!IsSupportedTarget(Target))
			{
				return ForwardDiagnostic(OutError, UnsupportedTarget(Target), Target.GetPathName());
			}

			FString CanonicalUsage;
			ENiagaraScriptUsage Usage = ENiagaraScriptUsage::Function;
			if (!UeAgentPaletteSearch::TryCanonicalizeNiagaraTargetUsage(
					Requested.TargetUsage, CanonicalUsage)
				|| !TryParseScriptUsage(CanonicalUsage, Usage))
			{
				auto Diagnostic = CandidateDiagnostic(TEXT("niagara_candidate_target_usage_invalid"), Target.GetPathName(),
					TEXT("Target usage must be a supported ENiagaraScriptUsage name or alias."));
				Diagnostic.TargetPath = Target.GetPathName();
				Diagnostic.SourceKind = Requested.SourceKind;
				Diagnostic.Source = Requested.TargetUsageSource;
				Diagnostic.PropertyPath = Requested.TargetUsageSource.JsonPath;
				Diagnostic.ActualValue = MakeShared<FJsonValueString>(Requested.TargetUsage);
				TArray<TSharedPtr<FJsonValue>> Allowed;
				const UEnum* Enum = StaticEnum<ENiagaraScriptUsage>();
				for (int32 Index = 0; Enum && Index < Enum->NumEnums(); ++Index)
				{
					const FString Name = Enum->GetNameStringByIndex(Index);
					ENiagaraScriptUsage Parsed;
					if (!Enum->HasMetaData(TEXT("Hidden"), Index) && TryParseScriptUsage(Name, Parsed))
						Allowed.Add(MakeShared<FJsonValueString>(Name));
				}
				Diagnostic.ExpectedValue = MakeShared<FJsonValueArray>(MoveTemp(Allowed));
				return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), Target.GetPathName());
			}

			FGuid RequestedUsageId;
			FGuid RequestedEmitterHandleId;
			bool bUsageIdExplicit = false;
			bool bEmitterHandleIdExplicit = false;
			if (!ReadGuid(
					Requested.TargetUsageId,
					TEXT("niagara_candidate_target_usage_id_invalid"),
					TEXT("target_usage_id"),
					RequestedUsageId,
					bUsageIdExplicit,
					OutError)
				|| !ReadGuid(
					Requested.EmitterHandleId,
					TEXT("niagara_candidate_emitter_handle_id_invalid"),
					TEXT("emitter_handle_id"),
					RequestedEmitterHandleId,
					bEmitterHandleIdExplicit,
					OutError))
			{
				return false;
			}

			TArray<FResolvedStage> Stages;
			UE::UAI::Contract::FDiagnostic StageError;
			if (!EnumerateStages(Target, Stages, StageError))
			{
				return ForwardDiagnostic(OutError, CandidateCause(TEXT("niagara_candidate_stage_enumeration_failed"), Target.GetPathName(),
					StageError, TEXT("Niagara stage enumeration failed without a diagnostic.")), Target.GetPathName());
			}

			TArray<const FResolvedStage*> Matches;
			for (const FResolvedStage& Stage : Stages)
			{
				if (Stage.Usage != Usage)
				{
					continue;
				}
				if (bUsageIdExplicit && Stage.UsageId != RequestedUsageId)
				{
					continue;
				}
				if (!Requested.EmitterName.IsEmpty()
					&& !Stage.EmitterName.Equals(Requested.EmitterName, ESearchCase::IgnoreCase))
				{
					continue;
				}
				if (!Requested.StageName.IsEmpty()
					&& !ScriptUsageToString(Stage.Usage).Equals(Requested.StageName, ESearchCase::IgnoreCase))
				{
					continue;
				}
				if (bEmitterHandleIdExplicit
					&& Stage.EmitterHandleId != RequestedEmitterHandleId)
				{
					continue;
				}
				Matches.Add(&Stage);
			}
			if (Matches.IsEmpty())
			{
				auto Diagnostic = ContextDiagnostic(TEXT("niagara_candidate_stage_not_found"),
					TEXT("No Niagara stage matches the requested context."), Target, Requested, false);
				Diagnostic.ExpectedValue = MakeShared<FJsonValueNumber>(1);
				Diagnostic.ActualValue = MakeShared<FJsonValueNumber>(0);
				return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), Target.GetPathName());
			}
			if (Matches.Num() != 1)
			{
				auto Diagnostic = ContextDiagnostic(TEXT("niagara_candidate_stage_ambiguous"),
					TEXT("More than one Niagara stage matches; supply an emitter or stage name that selects one."), Target, Requested, false);
				Diagnostic.ExpectedValue = MakeShared<FJsonValueNumber>(1);
				Diagnostic.ActualValue = MakeShared<FJsonValueNumber>(Matches.Num());
				return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), Target.GetPathName());
			}

			const FResolvedStage& Stage = *Matches[0];
			OutResolved.TargetUsage = ScriptUsageToString(Stage.Usage);
			OutResolved.EmitterName = Stage.EmitterName;
			OutResolved.StageName = OutResolved.TargetUsage;
			OutResolved.TargetUsageId = Stage.UsageId.ToString(
				EGuidFormats::DigitsWithHyphensLower);
			if (Stage.EmitterHandleId.IsValid())
			{
				OutResolved.EmitterHandleId = Stage.EmitterHandleId.ToString(
					EGuidFormats::DigitsWithHyphensLower);
			}
			OutResolved.StageKey = Stage.StageKey;
			OutResolved.Source = Requested.Source;
			OutResolved.TargetUsageSource = Requested.TargetUsageSource;
			OutResolved.SourceKind = Requested.SourceKind;
			return true;
		}

		bool ReadTargetRevision(
			UObject& Target,
			FString& OutRevision,
			FNiagaraModuleCandidateQueryError& OutError)
		{
			if (Target.GetOutermost() == GetTransientPackage())
			{
				OutRevision = Sha256(
					TEXT("transient|") + Target.GetClass()->GetPathName());
				return true;
			}
			FUeAgentContentGraphCapture Capture;
			FUeAgentContentGraphReadError ReadError;
			const bool bCaptured = FUeAgentContentGraphReadProductionEnvironment::TryCaptureOwner(Target, Capture, ReadError);
			if (!bCaptured || Capture.Revision.IsEmpty())
			{
				if (!ReadError.IsEmpty()) return ForwardDiagnostic(OutError, MoveTemp(ReadError), Target.GetPathName());
				auto Diagnostic = ContextDiagnostic(TEXT("niagara_candidate_target_readback_failed"),
					bCaptured ? TEXT("Niagara target readback did not return a revision.")
						: TEXT("Niagara target readback failed without a diagnostic."), Target, {});
				if (bCaptured)
				{
					Diagnostic.PropertyPath = TEXT("/revision");
					Diagnostic.ExpectedValuePresent = true;
					Diagnostic.ActualValuePresent = false;
				}
				return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), Target.GetPathName());
			}
			OutRevision = Capture.Revision;
			return true;
		}

		struct FNativeActionRecord
		{
			FNiagaraModuleCandidate Candidate;
			TSharedPtr<INiagaraStackItemGroupAddAction> Action;
		};

		struct FNativeActionSet
		{
			TSharedPtr<FNiagaraSystemViewModel> SystemViewModel;
			UNiagaraStackScriptItemGroup* Group = nullptr;
			INiagaraStackItemGroupAddUtilities* Utilities = nullptr;
			TArray<FNativeActionRecord> Records;
		};

		FString ActionSignature(
			const INiagaraStackItemGroupAddAction& Action,
			const FString& NativeActionKind,
			const FString& AssetPath)
		{
			TArray<FString> Parts;
			Parts.Add(TEXT("action_kind=") + NativeActionKind);
			if (NativeActionKind == TEXT("new_scratch_pad_module")
				|| NativeActionKind == TEXT("assignment_or_set_specific"))
			{
				return Parts[0];
			}
			if (!AssetPath.IsEmpty())
			{
				Parts.Add(TEXT("asset_path=") + AssetPath);
			}
			if (const TOptional<FNiagaraFavoritesActionData> Favorites =
					Action.GetFavoritesData();
				Favorites.IsSet())
			{
				for (const FName Name : Favorites->ActionIdentifier.Names)
				{
					Parts.Add(TEXT("favorite_name=") + Name.ToString());
				}
				for (const FGuid& Guid : Favorites->ActionIdentifier.Guids)
				{
					Parts.Add(TEXT("favorite_guid=")
						+ Guid.ToString(EGuidFormats::DigitsWithHyphensLower));
				}
			}
			if (Parts.Num() == 1)
			{
				// Last-resort identity for a native action that exposes no favorite id.
				Parts.Add(TEXT("display=") + Action.GetDisplayName().ToString());
				Parts.Add(TEXT("categories=")
					+ FString::Join(Action.GetCategories(), TEXT("/")));
				Parts.Add(TEXT("source=")
					+ Action.GetSourceData().SourceText.ToString());
			}
			return FString::Join(Parts, TEXT("|"));
		}

		FString ScriptAssetPath(
			const INiagaraStackItemGroupAddAction& Action)
		{
			const TOptional<FNiagaraFavoritesActionData> Favorites =
				Action.GetFavoritesData();
			if (!Favorites.IsSet())
			{
				return FString();
			}
			FAssetRegistryModule& AssetRegistry =
				FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
					TEXT("AssetRegistry"));
			for (const FName Name : Favorites->ActionIdentifier.Names)
			{
				const FString ObjectPath = Name.ToString();
				const FAssetData Asset = AssetRegistry.Get().GetAssetByObjectPath(
					FSoftObjectPath(ObjectPath));
				if (Asset.IsValid()
					&& Asset.IsInstanceOf(UNiagaraScript::StaticClass()))
				{
					return Asset.GetSoftObjectPath().ToString();
				}
			}
			return FString();
		}

		FString ActionKind(
			const INiagaraStackItemGroupAddAction& Action,
			const FString& AssetPath,
			int32& InOutUnboundActionOrdinal)
		{
			if (!AssetPath.IsEmpty())
			{
				return AssetModuleActionKind;
			}
			const FString Source = Action.GetSourceData().SourceText.ToString();
			if (Source.Equals(TEXT("Scratch Pad"), ESearchCase::IgnoreCase))
			{
				return TEXT("scratch_pad_module");
			}

			// UE 5.6 FScriptItemGroupAddUtilities appends these two semantic
			// actions, in this order, after asset and scratch-pad modules and
			// before module wizards.  Display text is localized and cannot own
			// action identity.
			const int32 UnboundActionOrdinal = InOutUnboundActionOrdinal++;
			if (UnboundActionOrdinal == 0)
			{
				return TEXT("new_scratch_pad_module");
			}
			if (UnboundActionOrdinal == 1)
			{
				return TEXT("assignment_or_set_specific");
			}
			return TEXT("module_wizard");
		}

		FString PublicActionName(
			const INiagaraStackItemGroupAddAction& Action,
			const FString& NativeActionKind,
			const FString& AssetPath)
		{
			if (!AssetPath.IsEmpty())
			{
				FString AssetName = FPackageName::ObjectPathToObjectName(AssetPath);
				if (AssetName.IsEmpty())
				{
					AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
				}
				if (!AssetName.IsEmpty())
				{
					return FName::NameToDisplayString(AssetName, false);
				}
			}
			const FText DisplayText = Action.GetDisplayName();
			const FString* const SourceText =
				FTextInspector::GetSourceString(DisplayText);
			FString ReadableName = SourceText
				? SourceText->TrimStartAndEnd()
				: DisplayText.ToString().TrimStartAndEnd();
			if (!ReadableName.IsEmpty()
				&& !ReadableName.Contains(FString::Chr(TCHAR(0xfffd))))
			{
				return ReadableName;
			}
			return FName::NameToDisplayString(NativeActionKind, false);
		}

		FString AssetOwnerScope(const FString& AssetPath)
		{
			const FString PackageName =
				FPackageName::ObjectPathToPackageName(AssetPath);
			TArray<FString> Parts;
			PackageName.ParseIntoArray(Parts, TEXT("/"), true);
			if (Parts.IsEmpty())
			{
				return FString();
			}
			const FString Root = FName::NameToDisplayString(Parts[0], false);
			const FString Parent = Parts.Num() > 1
				? FName::NameToDisplayString(Parts[Parts.Num() - 2], false)
				: Root;
			return Root.Equals(Parent, ESearchCase::IgnoreCase)
				? Root : Root + TEXT(" / ") + Parent;
		}

		bool ReadResolvedContext(const UObject& Target, const FNiagaraModuleCandidateContext& Context,
			ENiagaraScriptUsage& OutUsage, FGuid& OutUsageId, FNiagaraModuleCandidateQueryError& OutError)
		{
			if (!TryParseScriptUsage(Context.TargetUsage, OutUsage))
			{
				auto Diagnostic = ContextDiagnostic(TEXT("niagara_candidate_resolved_context_invalid"),
					TEXT("The resolved native stage has an invalid ENiagaraScriptUsage value."), Target, Context);
				Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("ENiagaraScriptUsage"));
				Diagnostic.ActualValue = MakeShared<FJsonValueString>(Context.TargetUsage);
				return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), Context.StageKey);
			}
			if (!FGuid::Parse(Context.TargetUsageId, OutUsageId))
			{
				auto Diagnostic = ContextDiagnostic(TEXT("niagara_candidate_resolved_context_invalid"),
					TEXT("The resolved native stage usage ID is not a valid GUID."), Target, Context);
				Diagnostic.Source.InternalJsonPath = TEXT("/target_usage_id");
				Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("GUID"));
				Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(Context.TargetUsageId);
				return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), Context.StageKey);
			}
			return true;
		}

		bool BuildNativeActionSet(
			UObject& Target,
			const FNiagaraModuleCandidateContext& ResolvedContext,
			FNativeActionSet& OutSet,
			FNiagaraModuleCandidateQueryError& OutError)
		{
			OutSet = {};
			UNiagaraSystem* System = Cast<UNiagaraSystem>(&Target);
			UNiagaraEmitter* const Emitter = Cast<UNiagaraEmitter>(&Target);
			if (!System && !Emitter)
			{
				return ForwardDiagnostic(OutError, UnsupportedTarget(Target), Target.GetPathName());
			}
			if (Emitter)
			{
				System = NewObject<UNiagaraSystem>(
					GetTransientPackage(),
					NAME_None,
					RF_Transient | RF_Transactional);
				UNiagaraSystemFactoryNew::InitializeSystem(System, true);
				System->EnsureFullyLoaded();
			}

			const FGuid MessageLogGuid = System->GetAssetGuid();
			if (!MessageLogGuid.IsValid())
			{
				auto Diagnostic = ContextDiagnostic(TEXT("niagara_candidate_asset_guid_missing"),
					TEXT("The Niagara system has no valid asset GUID for native candidate enumeration."), Target, ResolvedContext);
				Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("valid GUID"));
				Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(MessageLogGuid.ToString());
				return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), Target.GetPathName());
			}

			FNiagaraSystemViewModelOptions Options;
			Options.bCanModifyEmittersFromTimeline = false;
			Options.bCanAutoCompile = false;
			Options.bCanSimulate = false;
			Options.bIsForDataProcessingOnly = true;
			Options.bCompileForEdit = false;
			Options.MessageLogGuid = MessageLogGuid;
			Options.EditMode = Emitter
				? ENiagaraSystemViewModelEditMode::EmitterAsset
				: ENiagaraSystemViewModelEditMode::SystemAsset;
			OutSet.SystemViewModel = MakeShared<FNiagaraSystemViewModel>();
			OutSet.SystemViewModel->Initialize(*System, Options);
			TSharedPtr<FNiagaraEmitterHandleViewModel> PlaceholderEmitterViewModel;
			if (Emitter)
			{
				OutSet.SystemViewModel->GetEditorData()
					.SetOwningSystemIsPlaceholder(true, *System);
				PlaceholderEmitterViewModel = OutSet.SystemViewModel->AddEmitter(
					*Emitter,
					Emitter->GetExposedVersion().VersionGuid);
				if (!PlaceholderEmitterViewModel.IsValid())
				{
					return ForwardDiagnostic(OutError, ContextDiagnostic(TEXT("niagara_candidate_placeholder_emitter_failed"),
						TEXT("The native editor could not create the temporary emitter view model for candidate enumeration."), Target, ResolvedContext), Target.GetPathName());
				}
			}

			UNiagaraStackViewModel* StackViewModel = nullptr;
			if (Emitter)
			{
				StackViewModel =
					PlaceholderEmitterViewModel->GetEmitterStackViewModel();
			}
			else if (!ResolvedContext.EmitterHandleId.IsEmpty())
			{
				FGuid EmitterHandleId;
				FGuid::Parse(
					ResolvedContext.EmitterHandleId, EmitterHandleId);
				const TSharedPtr<FNiagaraEmitterHandleViewModel> EmitterViewModel =
					OutSet.SystemViewModel->GetEmitterHandleViewModelById(
						EmitterHandleId);
				StackViewModel = EmitterViewModel.IsValid()
					? EmitterViewModel->GetEmitterStackViewModel()
					: nullptr;
			}
			else
			{
				StackViewModel = OutSet.SystemViewModel->GetSystemStackViewModel();
			}
			UNiagaraStackEntry* const Root = StackViewModel
				? StackViewModel->GetRootEntry()
				: nullptr;
			if (!Root)
			{
				auto Diagnostic = ContextDiagnostic(TEXT("niagara_candidate_native_stack_unavailable"),
					StackViewModel ? TEXT("The native Niagara stack view model has no root entry.")
						: TEXT("The native Niagara stack view model is unavailable."), Target, ResolvedContext);
				Diagnostic.PropertyPath = StackViewModel ? TEXT("/stack/root") : TEXT("/stack");
				Diagnostic.ExpectedValuePresent = true;
				Diagnostic.ActualValuePresent = false;
				return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), Target.GetPathName());
			}

			ENiagaraScriptUsage Usage = ENiagaraScriptUsage::Function;
			FGuid UsageId;
			if (!ReadResolvedContext(Target, ResolvedContext, Usage, UsageId, OutError)) return false;
			TArray<UNiagaraStackScriptItemGroup*> Groups;
			Root->GetUnfilteredChildrenOfType<UNiagaraStackScriptItemGroup>(
				Groups, true);
			TArray<UNiagaraStackScriptItemGroup*> Matches;
			for (UNiagaraStackScriptItemGroup* Group : Groups)
			{
				if (Group
					&& Group->GetScriptUsage() == Usage
					&& Group->GetScriptUsageId() == UsageId)
				{
					Matches.Add(Group);
				}
			}
			if (Matches.Num() != 1)
			{
				auto Diagnostic = ContextDiagnostic(Matches.IsEmpty() ? TEXT("niagara_candidate_native_stack_not_found")
					: TEXT("niagara_candidate_native_stack_ambiguous"),
					Matches.IsEmpty() ? TEXT("No native stack group matches the resolved Niagara stage.")
						: TEXT("Multiple native stack groups match the resolved Niagara stage."), Target, ResolvedContext);
				Diagnostic.ExpectedValue = MakeShared<FJsonValueNumber>(1);
				Diagnostic.ActualValue = MakeShared<FJsonValueNumber>(Matches.Num());
				return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), ResolvedContext.StageKey);
			}
			OutSet.Group = Matches[0];
			OutSet.Utilities = OutSet.Group->GetAddUtilities();
			if (!OutSet.Utilities)
			{
				return ForwardDiagnostic(OutError, ContextDiagnostic(TEXT("niagara_candidate_native_add_utilities_missing"),
					TEXT("The native Niagara stack group has no add-action utilities."), Target, ResolvedContext), ResolvedContext.StageKey);
			}

			FNiagaraStackItemGroupAddOptions AddOptions;
			AddOptions.bIncludeDeprecated = false;
			AddOptions.bIncludeNonLibrary = true;
			TArray<TSharedRef<INiagaraStackItemGroupAddAction>> Actions;
			OutSet.Utilities->GenerateAddActions(Actions, AddOptions);
			int32 UnboundActionOrdinal = 0;
			for (const TSharedRef<INiagaraStackItemGroupAddAction>& Action
				: Actions)
			{
				const FString AssetPath = ScriptAssetPath(*Action);
				const FString NativeActionKind = ActionKind(
					*Action, AssetPath, UnboundActionOrdinal);
				const FString Signature = ActionSignature(
					*Action, NativeActionKind, AssetPath);
				UeAgentPaletteSearch::FCandidate IdentityCandidate;
				IdentityCandidate.Kind = CandidateKind;
				IdentityCandidate.Name = PublicActionName(
					*Action, NativeActionKind, AssetPath);
				IdentityCandidate.DisplayName = IdentityCandidate.Name;
				IdentityCandidate.FullName = Signature;
				IdentityCandidate.AuthoringSelectors.Add(
					TEXT("native_action_signature"), Signature);
				UE::UAI::Contract::FDiagnostic IdentityError;
				if (!UeAgentPaletteSearch::SetCandidateIdentity(
						CandidateKind,
						Signature,
						Signature,
						IdentityCandidate,
						IdentityError))
				{
					return ForwardDiagnostic(OutError, CandidateCause(TEXT("niagara_candidate_identity_failed"), IdentityCandidate.Name,
						IdentityError, TEXT("Native Niagara action identity construction failed without a diagnostic.")), IdentityCandidate.Name);
				}

				FNativeActionRecord& Record = OutSet.Records.AddDefaulted_GetRef();
				Record.Action = Action;
				Record.Candidate.NativeCandidateName = IdentityCandidate.Name;
				Record.Candidate.Name = IdentityCandidate.Name;
				Record.Candidate.Label = IdentityCandidate.DisplayName;
				Record.Candidate.Kind = CandidateKind;
				Record.Candidate.ActionKind = NativeActionKind;
				Record.Candidate.SelectionFields.Add(TEXT("action_kind"), NativeActionKind);
				Record.Candidate.Category = FString::Join(
					Action->GetCategories(), TEXT("/"));
				if (!Record.Candidate.Category.IsEmpty())
				{
					Record.Candidate.Scopes.Add(Record.Candidate.Category);
				}
				const FString OwnerScope = AssetOwnerScope(AssetPath);
				if (!OwnerScope.IsEmpty())
				{
					Record.Candidate.Scopes.AddUnique(OwnerScope);
					Record.Candidate.SelectionFields.Add(TEXT("source"), OwnerScope);
				}
				Record.Candidate.Scopes.AddUnique(
					FName::NameToDisplayString(NativeActionKind, false));
				Record.Candidate.ScriptAssetPath = AssetPath;
				const FString ScriptSource = UE::UAI::ContentDiscovery::MakeReadableDiscoveryPath(AssetPath);
				if (!ScriptSource.IsEmpty()) Record.Candidate.SelectionFields.Add(TEXT("script_source"), ScriptSource);
				Record.Candidate.bSupported =
					NativeActionKind == AssetModuleActionKind;
				Record.Candidate.SupportReason =
					Record.Candidate.bSupported
						? TEXT("Replays the exact data-processing Stack action without opening an editor window.")
						: TEXT("Search-equivalent native action; creation requires a dedicated noninteractive HJSON contract before it can be executed.");
				if (!Record.Candidate.bSupported)
				{
					Record.Candidate.SupportDiagnostic = ContextDiagnostic(
						TEXT("niagara_candidate_action_unsupported"),
						TEXT("This native Niagara action has no module creation binding; the current creation contract supports asset_module actions."),
						Target, ResolvedContext);
					Record.Candidate.SupportDiagnostic.CandidateName = Record.Candidate.Name;
					Record.Candidate.SupportDiagnostic.PropertyPath = TEXT("action_kind");
					Record.Candidate.SupportDiagnostic.ActualValue = MakeShared<FJsonValueString>(NativeActionKind);
					Record.Candidate.SupportDiagnostic.ExpectedValue = MakeShared<FJsonValueString>(AssetModuleActionKind);
					Record.Candidate.SupportDiagnostic.InternalContext = Signature;
				}
				Record.Candidate.SupportedOperations =
					Record.Candidate.bSupported
						? TArray<FString>{TEXT("create")}
						: TArray<FString>{};
				Record.Candidate.NativeIdentity = MoveTemp(IdentityCandidate.Identity.CanonicalKey);
			}
			return true;
		}

		bool ResolveOriginalStage(
			UObject& Target,
			const FNiagaraModuleCandidateContext& ResolvedContext,
			UNiagaraNodeOutput*& OutOutputNode,
			FNiagaraModuleCandidateQueryError& OutError)
		{
			OutOutputNode = nullptr;
			ENiagaraScriptUsage Usage = ENiagaraScriptUsage::Function;
			FGuid UsageId;
			if (!ReadResolvedContext(Target, ResolvedContext, Usage, UsageId, OutError)) return false;

			TArray<FResolvedStage> Stages;
			UE::UAI::Contract::FDiagnostic StageError;
			if (!EnumerateStages(Target, Stages, StageError))
			{
				return ForwardDiagnostic(OutError, CandidateCause(TEXT("niagara_candidate_stage_enumeration_failed"), Target.GetPathName(),
					StageError, TEXT("Niagara stage enumeration failed without a diagnostic.")), Target.GetPathName());
			}
			int32 MatchCount = 0;
			for (const FResolvedStage& Stage : Stages)
			{
				if (Stage.Usage == Usage && Stage.UsageId == UsageId)
				{
					++MatchCount;
					OutOutputNode = Stage.OutputNode;
				}
			}
			if (MatchCount != 1)
			{
				auto Diagnostic = ContextDiagnostic(MatchCount == 0 ? TEXT("niagara_candidate_stage_not_found")
					: TEXT("niagara_candidate_stage_ambiguous"),
					MatchCount == 0 ? TEXT("The original Niagara stage no longer exists.")
						: TEXT("Multiple original Niagara stages have the resolved usage and usage ID."), Target, ResolvedContext);
				Diagnostic.ExpectedValue = MakeShared<FJsonValueNumber>(1);
				Diagnostic.ActualValue = MakeShared<FJsonValueNumber>(MatchCount);
				return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), ResolvedContext.StageKey);
			}
			if (!OutOutputNode)
			{
				return ForwardDiagnostic(OutError, ContextDiagnostic(TEXT("niagara_candidate_stage_output_missing"),
					TEXT("The resolved original Niagara stage has no output node."), Target, ResolvedContext), ResolvedContext.StageKey);
			}
			return true;
		}

		bool TrySelectNativeAction(
			const FNativeActionSet& ActionSet,
			const FString& CandidateName,
			const FNativeActionRecord*& OutRecord,
			FNiagaraModuleCandidateQueryError& OutError,
			const TMap<FString, FString>& Select)
		{
			OutRecord = nullptr;
			const FString Name = CandidateName.TrimStartAndEnd();
			if (Name.IsEmpty())
			{
				return ForwardDiagnostic(OutError, SelectorRequired(CandidateName));
			}

			TArray<ContentDiscovery::FNameSelectionEntry> Entries;
			Entries.Reserve(ActionSet.Records.Num());
			for (const FNativeActionRecord& Record : ActionSet.Records)
			{
				Entries.Add({Record.Candidate.Name, Record.Candidate.NativeIdentity, Record.Candidate.SelectionFields});
			}
			int32 ResolvedIndex = INDEX_NONE;
			UE::UAI::Contract::FDiagnostic ResolveError;
			if (!ContentDiscovery::TryResolveNameSelection(
					Entries, Name, Select, ResolvedIndex, ResolveError,
					ContentDiscovery::ENameComparison::IgnoreCase))
			{
				OutError = MoveTemp(ResolveError);
				if (OutError.Source.JsonPath.IsEmpty() && !OutError.PropertyPath.IsEmpty())
					OutError.Source.JsonPath = OutError.PropertyPath.StartsWith(TEXT("/")) ? OutError.PropertyPath
						: TEXT("/") + OutError.PropertyPath.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
				return false;
			}
			if (!ActionSet.Records.IsValidIndex(ResolvedIndex))
			{
				auto Diagnostic = CandidateDiagnostic(TEXT("niagara_candidate_resolved_index_invalid"), Name,
					FString::Printf(TEXT("Native candidate selection returned an index outside the %d available actions."), ActionSet.Records.Num()));
				Diagnostic.SourceKind = TEXT("native_result");
				Diagnostic.CandidateName = Name;
				Diagnostic.ActualValue = MakeShared<FJsonValueNumber>(ResolvedIndex);
				return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), Name);
			}
			OutRecord = &ActionSet.Records[ResolvedIndex];
			return true;
		}

		bool CollectCandidates(
			UObject& Target,
			const FNiagaraModuleCandidateContext& RequestedContext,
			FNiagaraModuleCandidateContext& OutResolvedContext,
			TArray<FNiagaraModuleCandidate>& OutCandidates,
			FNiagaraModuleCandidateQueryError& OutError)
		{
			OutCandidates.Reset();
			if (!ResolveContext(
					Target, RequestedContext, OutResolvedContext, OutError))
			{
				return false;
			}
			FNativeActionSet ActionSet;
			if (!BuildNativeActionSet(
					Target, OutResolvedContext, ActionSet, OutError))
			{
				return false;
			}
			for (const FNativeActionRecord& Record : ActionSet.Records)
			{
				OutCandidates.Add(Record.Candidate);
			}
			return true;
		}

		bool MatchesSearch(
			const FNiagaraModuleCandidate& Candidate,
			const FString& Query)
		{
			ContentDiscovery::FContentDiscoveryRequest Request;
			Request.Query = Query;
			Request.MatchMode =
				ContentDiscovery::EContentDiscoveryMatchMode::Text;
			int32 MatchScore = 0;
			FString MatchReason;
			return ContentDiscovery::TryScoreContentDiscoveryMatch(
				Request,
				Candidate.NativeCandidateName,
				Candidate.Name,
				{
					Candidate.Label,
					Candidate.Category,
					Candidate.ActionKind,
					Candidate.ScriptAssetPath,
					Candidate.SupportReason,
				},
				MatchScore,
				MatchReason);
		}
	}

	bool TryQueryNiagaraModuleCandidates(
		UObject& Target,
		const FString& Query,
		const FNiagaraModuleCandidateContext& Context,
		FNiagaraModuleCandidateQueryResult& OutResult,
		FNiagaraModuleCandidateQueryError& OutError)
	{
		OutResult = {};
		OutError = {};
		if (!IsInGameThread())
		{
			auto Diagnostic = ContextDiagnostic(TEXT("niagara_candidate_game_thread_required"),
				TEXT("Niagara candidate queries require the Editor game thread."), Target, Context);
			Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("game_thread"));
			Diagnostic.ActualValue = MakeShared<FJsonValueString>(TEXT("other_thread"));
			return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), Target.GetPathName());
		}
		OutResult.Target = Target.GetPathName();
		if (!ReadTargetRevision(Target, OutResult.TargetRevision, OutError))
		{
			return false;
		}

		TArray<FNiagaraModuleCandidate> Candidates;
		if (!CollectCandidates(
				Target, Context, OutResult.Context, Candidates, OutError))
		{
			return false;
		}
		TArray<int32> MatchedIndices;
		for (int32 CandidateIndex = 0; CandidateIndex < Candidates.Num(); ++CandidateIndex)
		{
			if (MatchesSearch(Candidates[CandidateIndex], Query))
			{
				MatchedIndices.Add(CandidateIndex);
			}
		}
		MatchedIndices.Sort(
			[&Candidates](const int32 LeftIndex, const int32 RightIndex)
			{
				const FNiagaraModuleCandidate& Left = Candidates[LeftIndex];
				const FNiagaraModuleCandidate& Right = Candidates[RightIndex];
				const int32 NameCompare = Left.Name.Compare(
					Right.Name, ESearchCase::IgnoreCase);
				return NameCompare != 0
					? NameCompare < 0
					: Left.NativeCandidateName < Right.NativeCandidateName;
			});
		OutResult.TotalMatches = MatchedIndices.Num();
		for (int32 MatchIndex = 0; MatchIndex < MatchedIndices.Num(); ++MatchIndex)
		{
			FNiagaraModuleCandidate Candidate =
				Candidates[MatchedIndices[MatchIndex]];
			OutResult.Candidates.Add(MoveTemp(Candidate));
		}
		return true;
	}

	bool TrySelectNiagaraModuleCandidate(
		UObject& Target,
		const FString& CandidateName,
		const FNiagaraModuleCandidateContext& Context,
		FNiagaraModuleCandidate& OutCandidate,
		FNiagaraModuleCandidateContext& OutResolvedContext,
		FNiagaraModuleCandidateQueryError& OutError,
		ContentDiscovery::FReadOnlyCandidateBatch* Batch,
		const TMap<FString, FString>& Select)
	{
		OutCandidate = {};
		OutResolvedContext = {};
		OutError = {};
		if (CandidateName.TrimStartAndEnd().IsEmpty())
		{
			return ForwardDiagnostic(OutError, SelectorRequired(CandidateName), Target.GetPathName());
		}

		if (!ResolveContext(Target, Context, OutResolvedContext, OutError))
		{
			return false;
		}
		FString ContextKey;
		for (const FString* Part : {&OutResolvedContext.TargetUsage, &OutResolvedContext.EmitterName,
			&OutResolvedContext.StageName, &OutResolvedContext.TargetUsageId,
			&OutResolvedContext.EmitterHandleId, &OutResolvedContext.StageKey})
			ContextKey += FString::FromInt(Part->Len()) + TEXT(":") + *Part;
		const auto Prepared = ContentDiscovery::FReadOnlyCandidateBatch::Prepare<FNativeActionSet>(
			Batch, &Target, ContextKey, [&](FNativeActionSet& Value)
		{
			return BuildNativeActionSet(Target, OutResolvedContext, Value, OutError);
		});
		if (!Prepared) return false;
		auto& ActionSet = *Prepared;
		const FNativeActionRecord* Match = nullptr;
		if (!TrySelectNativeAction(
				ActionSet, CandidateName, Match, OutError, Select))
		{
			OutError = UeAgentAuthoringDiagnostics::CandidateFailure(OutError.Code, CandidateName, Target.GetPathName(), FString::Printf(TEXT("Niagara usage '%s', emitter '%s', stage '%s'"),
					*OutResolvedContext.TargetUsage, *OutResolvedContext.EmitterName, *OutResolvedContext.StageName), TEXT("niagara.module.candidates"), OutError);
			return false;
		}
		OutCandidate = Match->Candidate;
		return true;
	}

	bool TryResolveNiagaraModuleCandidate(UObject& Target, const FString& CandidateName,
		const FNiagaraModuleCandidateContext& Context, FNiagaraModuleCandidate& OutCandidate,
		FNiagaraModuleCandidateContext& OutResolvedContext, FNiagaraModuleCandidateQueryError& OutError,
		ContentDiscovery::FReadOnlyCandidateBatch* Batch,
		const TMap<FString, FString>& Select)
	{
		FNiagaraModuleCandidate Candidate;
		OutCandidate = {};
		if (!TrySelectNiagaraModuleCandidate(Target, CandidateName, Context, Candidate, OutResolvedContext, OutError, Batch, Select)) return false;
		if (!Candidate.bSupported)
		{
			return ForwardDiagnostic(OutError, Candidate.SupportDiagnostic, Candidate.NativeCandidateName);
		}
		OutCandidate = MoveTemp(Candidate);
		return true;
	}

	bool TryPerformNiagaraModuleCandidate(
		UObject& Target,
		const FString& CandidateName,
		const FNiagaraModuleCandidateContext& Context,
		const int32 TargetIndex,
		FNiagaraModuleCandidate& OutCandidate,
		FNiagaraModuleCandidateContext& OutResolvedContext,
		UNiagaraNodeFunctionCall*& OutCreatedNode,
		FNiagaraModuleCandidateQueryError& OutError,
		const TMap<FString, FString>& Select)
	{
		OutCandidate = {};
		OutResolvedContext = {};
		OutCreatedNode = nullptr;
		OutError = {};
		if (CandidateName.TrimStartAndEnd().IsEmpty())
		{
			return ForwardDiagnostic(OutError, SelectorRequired(CandidateName), Target.GetPathName());
		}
		if (!ResolveContext(Target, Context, OutResolvedContext, OutError))
		{
			return false;
		}

		FNativeActionSet ActionSet;
		if (!BuildNativeActionSet(
				Target, OutResolvedContext, ActionSet, OutError))
		{
			return false;
		}
		const FNativeActionRecord* Match = nullptr;
		if (!TrySelectNativeAction(
				ActionSet, CandidateName, Match, OutError, Select))
		{
			return false;
		}
		if (!Match->Candidate.bSupported)
		{
			return ForwardDiagnostic(OutError, Match->Candidate.SupportDiagnostic, Match->Candidate.NativeCandidateName);
		}
		if (!Match->Action.IsValid())
		{
			auto Diagnostic = ContextDiagnostic(TEXT("niagara_candidate_action_unsupported"),
				TEXT("The resolved native Niagara action payload is unavailable."), Target, OutResolvedContext);
			Diagnostic.CandidateName = Match->Candidate.Name;
			Diagnostic.ExpectedValuePresent = true;
			Diagnostic.ActualValuePresent = false;
			return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), Match->Candidate.NativeCandidateName);
		}

		UNiagaraNodeOutput* const OutputNode =
			ActionSet.Group ? ActionSet.Group->GetScriptOutputNode() : nullptr;
		if (!OutputNode || !ActionSet.Utilities)
		{
			return ForwardDiagnostic(OutError, ContextDiagnostic(TEXT("niagara_candidate_native_stack_unavailable"),
				!OutputNode ? TEXT("The native Niagara stack group has no script output node.")
					: TEXT("The native Niagara stack group has no add-action utilities."), Target, OutResolvedContext), OutResolvedContext.StageKey);
		}
		TArray<UNiagaraNodeFunctionCall*> BeforeModules;
		if (!GetModules(*OutputNode, BeforeModules, OutError)) return false;
		TSet<FGuid> BeforeGuids;
		for (const UNiagaraNodeFunctionCall* Module : BeforeModules)
		{
			if (Module)
			{
				BeforeGuids.Add(Module->NodeGuid);
			}
		}

		ActionSet.Utilities->ExecuteAddAction(
			Match->Action.ToSharedRef(), TargetIndex);
		TArray<UNiagaraNodeFunctionCall*> AfterModules;
		if (!GetModules(*OutputNode, AfterModules, OutError))
		{
			if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Target.GetPathName();
			if (!Target.IsA<UNiagaraEmitter>()) OutError.MutationStarted = true;
			return false;
		}
		TArray<UNiagaraNodeFunctionCall*> CreatedModules;
		for (UNiagaraNodeFunctionCall* Module : AfterModules)
		{
			if (Module && !BeforeGuids.Contains(Module->NodeGuid))
			{
				CreatedModules.Add(Module);
			}
		}
		if (CreatedModules.Num() != 1)
		{
			auto Diagnostic = ContextDiagnostic(TEXT("niagara_candidate_native_action_result_invalid"),
				TEXT("The native add action must produce exactly one new Niagara module."), Target, OutResolvedContext);
			Diagnostic.CandidateName = Match->Candidate.Name;
			Diagnostic.ExpectedValue = MakeShared<FJsonValueNumber>(1);
			Diagnostic.ActualValue = MakeShared<FJsonValueNumber>(CreatedModules.Num());
			if (!Target.IsA<UNiagaraEmitter>()) Diagnostic.MutationStarted = true;
			return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), Match->Candidate.NativeCandidateName);
		}
		if (Target.IsA<UNiagaraEmitter>())
		{
			const int32 NativeTargetIndex = AfterModules.IndexOfByKey(
				CreatedModules[0]);
			UNiagaraScript* const ModuleScript = CreatedModules[0]
				? CreatedModules[0]->FunctionScript.Get()
				: nullptr;
			if (NativeTargetIndex == INDEX_NONE || !ModuleScript)
			{
				auto Diagnostic = ContextDiagnostic(TEXT("niagara_candidate_native_action_result_invalid"),
					NativeTargetIndex == INDEX_NONE ? TEXT("The preview module is absent from the native stack after creation.")
						: TEXT("The preview module has no Niagara script."), Target, OutResolvedContext);
				Diagnostic.CandidateName = Match->Candidate.Name;
				if (NativeTargetIndex == INDEX_NONE) Diagnostic.ActualValue = MakeShared<FJsonValueNumber>(NativeTargetIndex);
				else { Diagnostic.ExpectedValuePresent = true; Diagnostic.ActualValuePresent = false; }
				return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), Match->Candidate.NativeCandidateName);
			}

			UNiagaraNodeOutput* OriginalOutputNode = nullptr;
			if (!ResolveOriginalStage(
					Target, OutResolvedContext, OriginalOutputNode, OutError))
			{
				return false;
			}
			UE::UAI::Contract::FDiagnostic AddError;
			OutCreatedNode = AddModule(
				*OriginalOutputNode,
				*ModuleScript,
				FString(),
				NativeTargetIndex,
				AddError);
			if (!OutCreatedNode)
			{
				return ForwardDiagnostic(OutError, CandidateCause(TEXT("niagara_candidate_native_action_failed"), Match->Candidate.NativeCandidateName,
					AddError, TEXT("The native Niagara module add operation failed without a diagnostic.")), Match->Candidate.NativeCandidateName);
			}
		}
		else
		{
			OutCreatedNode = CreatedModules[0];
		}
		OutCandidate = Match->Candidate;
		return true;
	}
}

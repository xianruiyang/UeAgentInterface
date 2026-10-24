#include "Composition/GraphCompound/UeAgentGraphCompoundNiagaraTypedMutationProduction.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundCanonical.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringPackageReadback.h"

#include "Core/AssetDurable/UeAgentAssetDurable.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Core/Contract/UeAgentExecutionContextPolicy.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Core/Verification/UeAgentIndependentReadback.h"
#include "DomainAdapters/GenericEffect/UeAgentGenericEffectAdapter.h"
#include "DomainAdapters/GenericRead/UeAgentGenericReadAdapter.h"
#include "DomainAdapters/MaterialNiagaraGraph/UeAgentMaterialNiagaraGraphNativeHook.h"
#include "DomainAdapters/NiagaraCompound/UeAgentNiagaraCompoundAdapter.h"
#include "DomainAdapters/NiagaraCompound/UeAgentNiagaraCompoundDiscovery.h"
#include "DomainAdapters/NiagaraCompound/UeAgentNiagaraCompoundNativeHook.h"
#include "Domains/Niagara/Authoring/UeAgentNiagaraStageModuleAuthoring.h"
#include "Domains/Niagara/UeAgentNiagaraCompileValidation.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformTime.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraSystem.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "ViewModels/NiagaraEmitterHandleViewModel.h"
#include "ViewModels/NiagaraSystemViewModel.h"
#include "ViewModels/Stack/NiagaraStackEntry.h"
#include "ViewModels/Stack/NiagaraStackModuleItem.h"
#include "ViewModels/Stack/NiagaraStackViewModel.h"

namespace UE::UAI::Composition::GraphCompound
{
	using namespace AssetDurable;
	using namespace ContextResource;
	using namespace Contract;
	using namespace Discovery;
	using namespace DomainAdapters::GenericEffect;
	using namespace DomainAdapters::GenericRead;
	using namespace DomainAdapters::MaterialNiagaraGraph;
	using namespace DomainAdapters::NiagaraCompound;
	using namespace ExecutionMutation;
	using namespace PlanningPreflight;
	using namespace StateSemantic;
	using namespace UeAgentNiagaraStageModuleAuthoring;
	using namespace Verification;

	namespace
	{
        void SetError(FGraphCompoundRuntimeError& OutError, UE::UAI::Contract::FDiagnostic Diagnostic,
            FString SubjectId = FString(), const UE::UAI::Contract::FDiagnostic& Recovery = UE::UAI::Contract::FDiagnostic())
        {
            OutError = MoveTemp(Diagnostic);
            if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = MoveTemp(SubjectId);
            OutError.Append(Recovery);
        }

		FGraphCompoundRuntimeError TypedNiagaraFailure(const FString& Code, const FString& Reason,
			const FString& Target, const FString& Field, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
		{
			auto Error = FDiagnostic::Failure(Code, Reason, TEXT("graph_compound"));
			Error.TargetPath = Target;
			Error.PropertyPath = Field;
			Error.ActualValuePresent = Actual.IsValid();
			Error.ExpectedValuePresent = Expected.IsValid();
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			MarkGraphCompoundNativeDiagnostic(Error);
			return Error;
		}

		FGraphCompoundRuntimeError SelectionMatches(const FString& Code, const FString& Reason,
			const FGraphCompoundNiagaraMutationSelector& Selector, const FString& Field, const FString& Ref, const int32 Count)
		{
			const auto Actual = MakeShared<FJsonObject>();
			Actual->SetStringField(TEXT("reference"), Ref);
			Actual->SetNumberField(TEXT("match_count"), Count);
			if (!Selector.GetEmitterHandleId().IsEmpty()) Actual->SetStringField(TEXT("emitter_handle_id"), Selector.GetEmitterHandleId());
			if (!Selector.GetStageRef().IsEmpty()) Actual->SetStringField(TEXT("stage_ref"), Selector.GetStageRef());
			if (!Selector.GetRendererRef().IsEmpty()) Actual->SetStringField(TEXT("renderer_ref"), Selector.GetRendererRef());
			const auto Expected = MakeShared<FJsonObject>();
			Expected->SetNumberField(TEXT("match_count"), 1);
			auto Error = TypedNiagaraFailure(Code, Reason, Selector.GetTargetRef(), Field,
				MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueObject>(Expected));
			Error.SubjectId = Selector.GetStableSemanticId();
			return Error;
		}

		const FString NiagaraTypedReadbackOracleId =
			TEXT("uai.readback.graph_compound.niagara_typed_mutation");
		const FString NiagaraCompoundGeneration =
			TEXT("graph-compound-niagara-typed-v1");

		

		FString PackageNameFromRef(const FString& Ref)
		{
			return Ref.Contains(TEXT("."))
				? FPackageName::ObjectPathToPackageName(Ref)
				: Ref;
		}

		FString ObjectPathFromRef(const FString& Ref)
		{
			const FString PackageName = PackageNameFromRef(Ref);
			return PackageName + TEXT(".")
				+ FPackageName::GetLongPackageAssetName(PackageName);
		}

		UNiagaraSystem* TryResolveSystem(
			const FString& TargetRef,
			FGraphCompoundRuntimeError& OutError)
		{
			const FString PackageName = PackageNameFromRef(TargetRef);
			const bool bValidPackage = FPackageName::IsValidLongPackageName(PackageName);
			UNiagaraSystem* const System = bValidPackage
				? LoadObject<UNiagaraSystem>(nullptr, *ObjectPathFromRef(PackageName)) : nullptr;
			if (!bValidPackage)
			{
				OutError = TypedNiagaraFailure(TEXT("graph_compound_niagara_system_invalid"),
					TEXT("The Niagara system target does not contain a valid long package name."), TargetRef, TEXT("package_name"),
					MakeShared<FJsonValueString>(PackageName), MakeShared<FJsonValueString>(TEXT("valid long package name")));
				return nullptr;
			}
			if (!System)
			{
				OutError = TypedNiagaraFailure(TEXT("graph_compound_niagara_system_invalid"),
					TEXT("The referenced object could not be loaded as a Niagara system."), TargetRef, TEXT("object_path"),
					MakeShared<FJsonValueString>(ObjectPathFromRef(PackageName)), MakeShared<FJsonValueString>(TEXT("loadable NiagaraSystem")));
				return nullptr;
			}
			if (System->GetOutermost()->GetName() != PackageName)
			{
				OutError = TypedNiagaraFailure(TEXT("graph_compound_niagara_system_invalid"),
					TEXT("The loaded Niagara system belongs to a different package."), TargetRef, TEXT("asset.package"),
					MakeShared<FJsonValueString>(System->GetOutermost()->GetName()), MakeShared<FJsonValueString>(PackageName));
				return nullptr;
			}
			return System;
		}


		FUeAgentCanonicalIdentity Identity(
			const FGraphCompoundDomainRouteProfile& Profile,
			const FGraphCompoundNiagaraMutationSelector& Selector,
			const FString& Generation)
		{
			const FString SemanticId = Selector.GetStableSemanticId();
			return FUeAgentCanonicalIdentity(
				SemanticId, Profile.GetTargetKind(), Selector.GetTargetRef(),
				SemanticId, TEXT("graph_compound"), Generation);
		}

		bool TryCanonicalText(
			const FUeAgentCanonicalValueRef& Value,
			FString& OutText,
			FGraphCompoundRuntimeError& OutError,
			const FString& Subject)
		{
			TSharedPtr<FJsonValue> JsonValue;
			UE::UAI::Contract::FDiagnostic Error;
			if (!CanonicalJson::TryToJsonValue(*Value, JsonValue, Error)
				|| !Json::TryCanonicalize(JsonValue, OutText, Error))
			{
				MarkGraphCompoundNativeDiagnostic(Error);
				SetError(OutError, Error, Subject);
				return false;
			}
			return true;
		}

		bool TryStringValue(
			const FString& Text,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FGraphCompoundRuntimeError& OutError,
			const FString& Subject)
		{
			FUeAgentStateSemanticError Error;
			if (!FUeAgentCanonicalValue::TryString(Text, Resources, OutValue, Error))
			{
				MarkGraphCompoundNativeDiagnostic(Error);
				SetError(OutError, Error, Subject);
				return false;
			}
			return true;
		}

		bool TryBuildSnapshot(
			const FGraphCompoundDomainRouteProfile& Profile,
			const FGraphCompoundNiagaraMutationSelector& Selector,
			const FUeAgentCanonicalValueRef& Current,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalSnapshotPtr& OutSnapshot,
			FGraphCompoundRuntimeError& OutError)
		{
			FString Canonical;
			if (!TryCanonicalText(
					Current, Canonical, OutError,
					Selector.GetStableSemanticId()))
			{
				return false;
			}
			const FString Generation = Json::Sha256Utf8(
				Selector.GetTargetRef() + TEXT("\n")
				+ Selector.GetStableSemanticId() + TEXT("\n") + Canonical);
			const FUeAgentStateRevision Revision(
				TEXT("graph-compound:c4:niagara-typed"), Generation, 0);
			const FUeAgentCanonicalIdentity EntryIdentity =
				Identity(Profile, Selector, Generation);
			FUeAgentStateEntry Entry(
				EntryIdentity,
				FUeAgentCanonicalType(
					Current->GetKind() == EUeAgentValueKind::Object
						? TEXT("uai.niagara.input.v1")
						: TEXT("uai.graph_compound.state.v1"),
					Current->GetKind(), false,
					Current->GetKind() == EUeAgentValueKind::Object
						? TEXT("object") : TEXT("scalar")),
				Current,
				FUeAgentDefaultValue(
					EUeAgentDefaultSourceKind::DomainNative,
					Selector.GetTargetRef(),
					TEXT("independent exact Niagara selector readback"),
					Current),
				FUeAgentStateDetails(
					Selector.GetStableSemanticId(),
					TEXT("GraphCompound exact Niagara typed state"),
					{TEXT("GraphCompound"), TEXT("Niagara"), TEXT("Typed")},
					true, TEXT("available"), {}),
				Revision,
				EUeAgentAuthoredPresence::Present,
				EUeAgentOverrideState::Overridden);
			if (!Entry.IsValid())
			{
				SetError(OutError, Entry.GetValidationDiagnostic(), Selector.GetStableSemanticId());
				MarkGraphCompoundNativeDiagnostic(OutError);
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Selector.GetTargetRef();
				return false;
			}
			FUeAgentStateSemanticError Error;
			if (!FUeAgentCanonicalSnapshot::TryCreate(
					TEXT("graph-compound:c4:niagara-typed:")
						+ Json::Sha256Utf8(Selector.GetStableSemanticId()),
					EUeAgentStatePlane::Persisted,
					Revision,
					FUeAgentTemporalPoint(
						TEXT("utc-seconds"),
						FString::Printf(TEXT("%lld"),
							FDateTime::UtcNow().ToUnixTimestamp())),
					FUeAgentStateProvenance(
						TEXT("independent_readback"),
						Selector.GetTargetRef(),
						TEXT("niagara-typed:") + Generation),
					{MoveTemp(Entry)}, Resources, OutSnapshot, Error))
			{
				MarkGraphCompoundNativeDiagnostic(Error);
				if (Error.TargetPath.IsEmpty()) Error.TargetPath = Selector.GetTargetRef();
				SetError(OutError, Error, Selector.GetTargetRef());
				return false;
			}
			return true;
		}

		bool TryParseModuleGuid(
			const FString& ModuleRef,
			FGuid& OutGuid)
		{
			return ModuleRef.StartsWith(TEXT("module:"))
				&& FGuid::Parse(ModuleRef.RightChop(7), OutGuid)
				&& OutGuid.IsValid();
		}

		bool TryResolveEmitterGuid(
			UNiagaraSystem& System,
			const FGraphCompoundNiagaraMutationSelector& Selector,
			FGuid& OutGuid,
			FGraphCompoundRuntimeError& OutError)
		{
			if (FGuid::Parse(Selector.GetEmitterHandleId(), OutGuid) && OutGuid.IsValid()) return true;
			FEmitterOwnerTarget Target;
			Target.AssetPath = Selector.GetTargetRef();
			Target.EmitterName = Selector.GetEmitterHandleId();
			FResolvedEmitterOwner Owner;
			UE::UAI::Contract::FDiagnostic Error;
			if (!ResolveEmitterOwner(Target, Owner, Error, &System))
			{
				SetError(OutError, Error, Selector.GetEmitterHandleId());
				return false;
			}
			OutGuid = Owner.EmitterHandleId;
			return true;
		}

		bool TryResolveStage(
			UNiagaraSystem& System,
			const FGraphCompoundNiagaraMutationSelector& Selector,
			FResolvedStage& OutStage,
			FGraphCompoundRuntimeError& OutError)
		{
			FGuid EmitterHandleId;
			if (!TryResolveEmitterGuid(System, Selector, EmitterHandleId, OutError)) return false;
			TArray<FResolvedStage> Stages;
			UE::UAI::Contract::FDiagnostic Error;
			if (!EnumerateStages(System, Stages, Error))
			{
				SetError(OutError, Error, Selector.GetTargetRef());
				return false;
			}
			const FResolvedStage* Match = nullptr;
			int32 MatchCount = 0;
			for (const FResolvedStage& Stage : Stages)
			{
				if (Stage.EmitterHandleId == EmitterHandleId
					&& Stage.StageKey == Selector.GetStageRef())
				{
					Match = &Stage;
					++MatchCount;
				}
			}
			if (MatchCount != 1)
			{
				OutError = SelectionMatches(TEXT("graph_compound_niagara_stage_not_unique"),
					TEXT("The stage reference must resolve to exactly one stage in the selected emitter."),
					Selector, TEXT("stage_ref"), Selector.GetStageRef(), MatchCount);
				return false;
			}
			if (!Match->OutputNode)
			{
				OutError = TypedNiagaraFailure(TEXT("graph_compound_niagara_stage_not_unique"),
					TEXT("The selected Niagara stage has no output node."), Selector.GetTargetRef(),
					TEXT("stage.output_node.present"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				OutError.CandidateName = Selector.GetStageRef();
				return false;
			}
			OutStage = *Match;
			return true;
		}

		bool TryCaptureReorder(
			UNiagaraSystem& System,
			const FGraphCompoundNiagaraMutationSelector& Selector,
			FMaterialNiagaraGraphNativeHook& NativeHook,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FGraphCompoundRuntimeError& OutError)
		{
			FResolvedStage Stage;
			if (!TryResolveStage(System, Selector, Stage, OutError))
			{
				return false;
			}
			FMaterialNiagaraGraphTopology Topology;
			FString Revision;
			FString Evidence;
			FMaterialNiagaraGraphAdapterError Error;
			if (!NativeHook.TryCaptureNiagaraStack(
					System, Stage.EmitterHandleId, Stage.Usage, Stage.UsageId,
					Topology, Revision, Evidence, Error))
			{
				if (Error.TargetPath.IsEmpty()) Error.TargetPath = Selector.GetTargetRef();
				SetError(OutError, Error, Selector.GetItemRef());
				return false;
			}
			const FMaterialNiagaraGraphOrderingIdentity* Match = nullptr;
			int32 MatchCount = 0;
			for (const FMaterialNiagaraGraphOrderingIdentity& Ordering
				: Topology.GetOrdering())
			{
				if (Ordering.GetItemKey() == Selector.GetItemRef())
				{
					Match = &Ordering;
					++MatchCount;
				}
			}
			if (MatchCount != 1)
			{
				OutError = SelectionMatches(TEXT("graph_compound_niagara_order_item_not_unique"),
					TEXT("The reorder item reference must resolve to exactly one ordering item in the selected stage."),
					Selector, TEXT("item_ref"), Selector.GetItemRef(), MatchCount);
				return false;
			}
			return TryStringValue(
				Match->GetPredecessorItemKey(), Resources, OutValue,
				OutError, Selector.GetItemRef());
		}

		bool TryIssueState(
			UNiagaraSystem& System,
			const FGraphCompoundNiagaraMutationSelector& Selector,
			const bool bApply,
			FString& OutState,
			FGraphCompoundRuntimeError& OutError)
		{
			OutState.Reset();
			FGuid EmitterHandleId;
			if (!TryResolveEmitterGuid(System, Selector, EmitterHandleId, OutError)) return false;
			TSharedPtr<FNiagaraSystemViewModel> ViewModel =
				MakeShared<FNiagaraSystemViewModel>();
			FNiagaraSystemViewModelOptions Options;
			Options.bCanModifyEmittersFromTimeline = false;
			Options.bCanAutoCompile = false;
			Options.bCanSimulate = false;
			Options.bIsForDataProcessingOnly = false;
			Options.bCompileForEdit = false;
			Options.EditMode = ENiagaraSystemViewModelEditMode::SystemAsset;
			Options.MessageLogGuid = System.GetAssetGuid();
			ViewModel->Initialize(System, Options);

			UNiagaraStackViewModel* StackViewModel = nullptr;
			int32 EmitterCount = 0;
			for (const TSharedRef<FNiagaraEmitterHandleViewModel>& Handle
				: ViewModel->GetEmitterHandleViewModels())
			{
				if (Handle->GetId() == EmitterHandleId)
				{
					StackViewModel = Handle->GetEmitterStackViewModel();
					++EmitterCount;
				}
			}
			UNiagaraStackEntry* Root = StackViewModel
				? StackViewModel->GetRootEntry() : nullptr;
			if (EmitterCount != 1 || !Root)
			{
				OutError = EmitterCount != 1
					? SelectionMatches(TEXT("graph_compound_niagara_issue_emitter_not_unique"),
						TEXT("The issue-fix emitter must resolve to exactly one emitter view model."),
						Selector, TEXT("emitter_handle_id"), Selector.GetEmitterHandleId(), EmitterCount)
					: TypedNiagaraFailure(TEXT("graph_compound_niagara_issue_emitter_not_unique"),
						TEXT("The selected emitter has no stack root for issue inspection."), Selector.GetTargetRef(),
						TEXT("emitter.stack_root.present"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				OutError.CandidateName = Selector.GetEmitterHandleId();
				ViewModel.Reset();
				return false;
			}

			Root->RefreshChildren();
			Root->RefreshFilteredChildren();
			TArray<UNiagaraStackEntry*> Entries{Root};
			Entries.Append(Root->GetAllChildrenWithIssues());
			TSet<const UNiagaraStackEntry*> Seen;
			int32 IssueCount = 0;
			int32 FixCount = 0;
			UNiagaraStackEntry::FStackIssueFixDelegate FixDelegate;
			for (UNiagaraStackEntry* Entry : Entries)
			{
				if (!Entry || Seen.Contains(Entry)) continue;
				Seen.Add(Entry);
				for (const UNiagaraStackEntry::FStackIssue& Issue
					: Entry->GetIssues())
				{
					if (Issue.GetUniqueIdentifier() != Selector.GetIssueId()
						&& Issue.GetShortDescription().ToString() != Selector.GetIssueId())
					{
						continue;
					}
					++IssueCount;
					for (const UNiagaraStackEntry::FStackIssueFix& Fix
						: Issue.GetFixes())
					{
						if ((Fix.GetUniqueIdentifier() == Selector.GetFixId()
							|| Fix.GetDescription().ToString() == Selector.GetFixId())
							&& Fix.IsValid()
							&& Fix.GetFixDelegate().IsBound())
						{
							++FixCount;
							FixDelegate = Fix.GetFixDelegate();
						}
					}
				}
			}
			if (IssueCount == 0)
			{
				ViewModel.Reset();
				OutState = TEXT("resolved");
				return true;
			}
			if (IssueCount != 1 || FixCount != 1 || !FixDelegate.IsBound())
			{
				if (IssueCount != 1)
					OutError = SelectionMatches(TEXT("graph_compound_niagara_issue_fix_not_unique"),
						TEXT("The issue reference matches more than one stack issue."),
						Selector, TEXT("issue_id"), Selector.GetIssueId(), IssueCount);
				else if (FixCount != 1)
					OutError = SelectionMatches(TEXT("graph_compound_niagara_issue_fix_not_unique"),
						TEXT("The fix reference must resolve to exactly one valid, bound fix for the selected issue."),
						Selector, TEXT("fix_id"), Selector.GetFixId(), FixCount);
				else
					OutError = TypedNiagaraFailure(TEXT("graph_compound_niagara_issue_fix_not_unique"),
						TEXT("The selected stack issue fix delegate is not bound."), Selector.GetTargetRef(),
						TEXT("fix.delegate.bound"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				OutError.CandidateName = Selector.GetIssueId();
				ViewModel.Reset();
				return false;
			}
			OutState = TEXT("unresolved");
			if (bApply)
			{
				FixDelegate.Execute();
			}
			ViewModel.Reset();
			return true;
		}

		bool TryFindCompoundBinding(
			const FGraphCompoundDomainRouteProfile& Profile,
			const FGraphCompoundNiagaraMutationSelector& Selector,
			FUeAgentResourceTracker& Resources,
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			TSharedPtr<const FUeAgentCandidateBinding>& OutBinding,
			FGraphCompoundRuntimeError& OutError)
		{
			OutBinding.Reset();
			TArray<FUeAgentCandidateBinding> Bindings;
			FUeAgentDiscoveryError Error;
			if (!TryDiscoverNiagaraCompoundBindings(
					ObjectPathFromRef(Selector.GetTargetRef()),
					Profile.GetExecutionContract().GetDescriptorIdentity(),
					NiagaraCompoundGeneration, Resources, Deadline,
					Cancellation, Bindings, Error))
			{
				OutError = Error;
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Error.SubjectId;
				return false;
			}
			const FString ModuleGuid = Selector.GetModuleRef().RightChop(7);
			int32 MatchCount = 0;
			int32 EmitterMatchCount = 0;
			int32 StageMatchCount = 0;
			int32 ModuleMatchCount = 0;
			int32 InputMatchCount = 0;
			TArray<FString> CandidateSamples;
			for (const FUeAgentCandidateBinding& Binding : Bindings)
			{
				FUeAgentNiagaraCompoundSelector Candidate;
				UE::UAI::Contract::FDiagnostic ParseError;
				if (TryParseNiagaraCompoundSelector(
						Binding.GetSummary().GetIdentity().GetSelector(),
						Candidate, ParseError))
				{
					const bool bEmitter = Candidate.EmitterHandleId
						== Selector.GetEmitterHandleId();
					const bool bStage = Candidate.StageKey
						== Selector.GetStageRef();
					const bool bModule = Candidate.ModuleNodeGuid == ModuleGuid;
					const bool bInput = Candidate.InputName
						== Selector.GetInputName();
					if (CandidateSamples.Num() < 4)
					{
						CandidateSamples.Add(
							Candidate.EmitterHandleId + TEXT("|")
							+ Candidate.StageKey + TEXT("|")
							+ Candidate.ModuleNodeGuid + TEXT("|")
							+ Candidate.InputName);
					}
					EmitterMatchCount += bEmitter ? 1 : 0;
					StageMatchCount += bStage ? 1 : 0;
					ModuleMatchCount += bModule ? 1 : 0;
					InputMatchCount += bInput ? 1 : 0;
					if (bEmitter && bStage && bModule && bInput)
					{
						OutBinding = MakeShared<const FUeAgentCandidateBinding>(
							Binding);
						++MatchCount;
					}
				}
			}
			if (MatchCount != 1)
			{
				const auto Actual = MakeShared<FJsonObject>();
				Actual->SetNumberField(TEXT("match_count"), MatchCount);
				Actual->SetNumberField(TEXT("candidate_count"), Bindings.Num());
				Actual->SetStringField(TEXT("emitter_handle_id"), Selector.GetEmitterHandleId());
				Actual->SetStringField(TEXT("stage_ref"), Selector.GetStageRef());
				Actual->SetStringField(TEXT("module_ref"), Selector.GetModuleRef());
				Actual->SetStringField(TEXT("input_name"), Selector.GetInputName());
				OutError = TypedNiagaraFailure(TEXT("graph_compound_niagara_compound_binding_not_unique"),
					TEXT("The compound input selector must resolve to exactly one candidate binding."), Selector.GetTargetRef(),
					TEXT("compound_input.selector"), MakeShared<FJsonValueObject>(Actual),
					MakeShared<FJsonValueString>(TEXT("exactly one binding matching emitter, stage, module and input")));
				OutError.SubjectId = Selector.GetStableSemanticId();
				OutError.InternalContext = FString::Printf(TEXT("emitter=%d,stage=%d,module=%d,input=%d,samples=%s"),
					EmitterMatchCount, StageMatchCount, ModuleMatchCount, InputMatchCount, *FString::Join(CandidateSamples, TEXT(";")));
				return false;
			}
			return true;
		}

		FGraphCompoundRuntimeError CompoundCaptureFailure(
			const TSharedPtr<const FUeAgentUeReadCapture>& Capture, FGraphCompoundRuntimeError Error,
			const FUeAgentCandidateBinding& Binding, bool bCaptureSucceeded)
		{
			if (Error.IsEmpty() && Capture.IsValid() && !Capture->GetValidationDiagnostic().IsEmpty())
				Error = FGraphCompoundRuntimeError::WrapFailure(TEXT("graph_compound_niagara_compound_capture_invalid"), Capture->GetValidationDiagnostic());
			if (Error.IsEmpty())
			{
				const bool bWrongCount = bCaptureSucceeded && Capture.IsValid() && Capture->GetEntries().Num() != 1;
				Error = FGraphCompoundRuntimeError::Failure(TEXT("graph_compound_niagara_compound_capture_invalid"),
					!bCaptureSucceeded ? TEXT("The native Niagara hook failed without supplying a diagnostic.")
						: !Capture.IsValid() ? TEXT("The native Niagara hook returned no capture.")
							: TEXT("The native Niagara capture must contain exactly one entry."), TEXT("native_graph"));
				Error.SourceKind = TEXT("native_result");
				Error.PropertyPath = !bCaptureSucceeded ? TEXT("/native_capture_succeeded") : bWrongCount ? TEXT("/entries/count") : TEXT("/capture_present");
				if (bWrongCount)
				{
					Error.ActualValue = MakeShared<FJsonValueNumber>(Capture->GetEntries().Num());
					Error.ExpectedValue = MakeShared<FJsonValueNumber>(1);
				}
				else
				{
					Error.ActualValue = MakeShared<FJsonValueBoolean>(false);
					Error.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				}
				Error.ActualValuePresent = true;
				Error.ExpectedValuePresent = true;
			}
			if (Error.TargetPath.IsEmpty()) Error.TargetPath = Binding.GetPublicRef();
			if (Error.SubjectId.IsEmpty()) Error.SubjectId = Binding.GetBindingId();
			return Error;
		}

		bool TryCaptureCompound(
			const FGraphCompoundDomainRouteProfile& Profile,
			const FGraphCompoundNiagaraMutationSelector& Selector,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FGraphCompoundRuntimeError& OutError)
		{
			const auto Clock = []() { return FPlatformTime::Seconds(); };
			FUeAgentCancellationSource Cancellation(Clock);
			const FUeAgentOperationDeadline Deadline;
			TSharedPtr<const FUeAgentCandidateBinding> Binding;
			if (!TryFindCompoundBinding(
					Profile, Selector, Resources, Deadline,
					Cancellation.GetToken(), Binding, OutError))
			{
				return false;
			}
			const TSharedRef<FUeAgentNiagaraCompoundNativeHook> Native =
				MakeShared<FUeAgentNiagaraCompoundNativeHook>(
					NiagaraCompoundGeneration);
			const FUeAgentStateReadRequest Request(
				*Binding, EUeAgentStatePlane::Persisted);
			TSharedPtr<const FUeAgentUeReadCapture> Capture;
			FUeAgentGenericReadError Error;
			const bool bCaptured = Native->TryCapture(
					Request, Resources, Deadline, Cancellation.GetToken(),
					Capture, Error);
			if (!bCaptured
				|| !Capture.IsValid() || Capture->GetEntries().Num() != 1)
			{
				OutError = CompoundCaptureFailure(Capture, MoveTemp(Error), *Binding, bCaptured);
				return false;
			}
			OutValue = Capture->GetEntries()[0].GetValue();
			return true;
		}

		bool TryResolveRendererBinding(
			UNiagaraSystem& System,
			const FGraphCompoundNiagaraMutationSelector& Selector,
			UNiagaraRendererProperties*& OutRenderer,
			FStructProperty*& OutProperty,
			FNiagaraVariableAttributeBinding*& OutBinding,
			FVersionedNiagaraEmitter& OutEmitter,
			FGraphCompoundRuntimeError& OutError)
		{
			OutRenderer = nullptr;
			OutProperty = nullptr;
			OutBinding = nullptr;
			FGuid EmitterHandleId;
			if (!TryResolveEmitterGuid(System, Selector, EmitterHandleId, OutError)) return false;
			FNiagaraEmitterHandle* Handle =
				System.GetEmitterHandles().FindByPredicate(
					[EmitterHandleId](const FNiagaraEmitterHandle& Candidate)
					{
						return Candidate.GetId() == EmitterHandleId;
					});
			FVersionedNiagaraEmitterData* Data = Handle
				? Handle->GetInstance().GetEmitterData() : nullptr;
			if (!Handle || !Data)
			{
				OutError = TypedNiagaraFailure(TEXT("graph_compound_niagara_renderer_emitter_missing"),
					!Handle ? TEXT("The renderer's emitter handle is absent from the Niagara system.")
						: TEXT("The renderer's emitter has no versioned data."), Selector.GetTargetRef(),
					!Handle ? TEXT("emitter_handle.present") : TEXT("emitter_data.present"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				OutError.CandidateName = Selector.GetEmitterHandleId();
				return false;
			}
			const FString RendererName = Selector.GetRendererRef().RightChop(9);
			int32 RendererCount = 0;
			for (UNiagaraRendererProperties* Renderer : Data->GetRenderers())
			{
				if (Renderer && Renderer->GetName() == RendererName)
				{
					OutRenderer = Renderer;
					++RendererCount;
				}
			}
			if (RendererCount != 1)
			{
				OutError = SelectionMatches(TEXT("graph_compound_niagara_renderer_not_unique"),
					TEXT("The renderer reference must resolve to exactly one renderer in the selected emitter."),
					Selector, TEXT("renderer_ref"), Selector.GetRendererRef(), RendererCount);
				return false;
			}
			int32 PropertyCount = 0;
			for (TFieldIterator<FStructProperty> It(OutRenderer->GetClass());
				It; ++It)
			{
				FStructProperty* Property = *It;
				if (!Property || Property->Struct
					!= FNiagaraVariableAttributeBinding::StaticStruct())
				{
					continue;
				}
				FString Slot = Property->GetName();
				if (Slot.EndsWith(TEXT("Binding")))
				{
					Slot.LeftChopInline(7);
				}
				Slot.ToLowerInline();
				if (Slot == Selector.GetParameterName())
				{
					OutProperty = Property;
					OutBinding = Property
						->ContainerPtrToValuePtr<
							FNiagaraVariableAttributeBinding>(OutRenderer);
					++PropertyCount;
				}
			}
			if (PropertyCount != 1 || !OutProperty || !OutBinding)
			{
				OutError = PropertyCount != 1
					? SelectionMatches(TEXT("graph_compound_niagara_renderer_binding_not_unique"),
						TEXT("The renderer parameter must resolve to exactly one attribute-binding property."),
						Selector, TEXT("parameter_name"), Selector.GetParameterName(), PropertyCount)
					: TypedNiagaraFailure(TEXT("graph_compound_niagara_renderer_binding_not_unique"),
						TEXT("The selected renderer attribute-binding property has no binding storage."), Selector.GetTargetRef(),
						TEXT("renderer.binding_storage.present"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				OutError.CandidateName = Selector.GetParameterName();
				return false;
			}
			FString NativeType = OutBinding->GetParamMapBindableVariable()
				.GetType().GetName();
			NativeType.ToLowerInline();
			if (NativeType.StartsWith(TEXT("niagara")))
			{
				NativeType.RightChopInline(7);
			}
			if (NativeType != Selector.GetTypeRef())
			{
				OutError = TypedNiagaraFailure(TEXT("graph_compound_niagara_renderer_type_mismatch"),
					TEXT("The renderer binding's normalized native type differs from type_ref."), Selector.GetTargetRef(),
					TEXT("renderer.binding.type"), MakeShared<FJsonValueString>(NativeType), MakeShared<FJsonValueString>(Selector.GetTypeRef()));
				OutError.CandidateName = Selector.GetParameterName();
				return false;
			}
			OutEmitter = Handle->GetInstance();
			return true;
		}

		bool TryCaptureRenderer(
			UNiagaraSystem& System,
			const FGraphCompoundNiagaraMutationSelector& Selector,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FGraphCompoundRuntimeError& OutError)
		{
			UNiagaraRendererProperties* Renderer = nullptr;
			FStructProperty* Property = nullptr;
			FNiagaraVariableAttributeBinding* Binding = nullptr;
			FVersionedNiagaraEmitter Emitter;
			if (!TryResolveRendererBinding(
					System, Selector, Renderer, Property, Binding,
					Emitter, OutError))
			{
				return false;
			}
			return TryStringValue(
				Binding->GetParamMapBindableVariable().GetName().ToString(),
				Resources, OutValue, OutError,
				Selector.GetStableSemanticId());
		}

		bool TryCaptureCurrentValue(
			const FGraphCompoundDomainRouteProfile& Profile,
			const FGraphCompoundNiagaraMutationSelector& Selector,
			FMaterialNiagaraGraphNativeHook& NativeHook,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FGraphCompoundRuntimeError& OutError)
		{
			OutValue.Reset();
			UNiagaraSystem* const System =
				TryResolveSystem(Selector.GetTargetRef(), OutError);
			if (!System)
			{
				return false;
			}
			switch (Selector.GetInputKind())
			{
			case EGraphCompoundTypedInputKind::NiagaraStackReorder:
				return TryCaptureReorder(
					*System, Selector, NativeHook, Resources,
					OutValue, OutError);
			case EGraphCompoundTypedInputKind::NiagaraStackIssueFix:
			{
				FString State;
				return TryIssueState(
						*System, Selector, false, State, OutError)
					&& TryStringValue(
						State, Resources, OutValue, OutError,
						Selector.GetIssueId());
			}
			case EGraphCompoundTypedInputKind::NiagaraCompoundInput:
				return TryCaptureCompound(
					Profile, Selector, Resources, OutValue, OutError);
			case EGraphCompoundTypedInputKind::NiagaraRendererEventParameter:
				return TryCaptureRenderer(
					*System, Selector, Resources, OutValue, OutError);
			default:
				OutError = TypedNiagaraFailure(TEXT("graph_compound_niagara_selector_kind_invalid"),
					TEXT("This Niagara selector kind is not supported by typed mutation."), Selector.GetTargetRef(), TEXT("input_kind"),
					MakeShared<FJsonValueNumber>(static_cast<int32>(Selector.GetInputKind())),
					MakeShared<FJsonValueString>(TEXT("NiagaraStackReorder, NiagaraStackIssueFix, NiagaraCompoundInput or NiagaraRendererEventParameter")));
				return false;
			}
		}

		bool TryApplyReorder(
			UNiagaraSystem& System,
			const FGraphCompoundNiagaraMutationSelector& Selector,
			FMaterialNiagaraGraphNativeHook& NativeHook,
			FGraphCompoundRuntimeError& OutError)
		{
			FResolvedStage Stage;
			if (!TryResolveStage(System, Selector, Stage, OutError))
			{
				return false;
			}
			FGuid ModuleGuid;
			if (!TryParseModuleGuid(Selector.GetItemRef(), ModuleGuid))
			{
				OutError = TypedNiagaraFailure(TEXT("graph_compound_niagara_module_ref_invalid"),
					TEXT("The reorder item reference must contain a valid, non-zero module GUID."),
					Selector.GetTargetRef(), TEXT("item_ref"), MakeShared<FJsonValueString>(Selector.GetItemRef()),
					MakeShared<FJsonValueString>(TEXT("module:<non-zero GUID>")));
				return false;
			}
			TArray<UNiagaraNodeFunctionCall*> Modules;
			if (!GetModules(*Stage.OutputNode, Modules, OutError)) return false;
			UNiagaraNodeFunctionCall* Module = nullptr;
			int32 SourceIndex = INDEX_NONE;
			for (int32 Index = 0; Index < Modules.Num(); ++Index)
			{
				if (Modules[Index] && Modules[Index]->NodeGuid == ModuleGuid)
				{
					if (Module)
					{
						TArray<TSharedPtr<FJsonValue>> MatchingIndices{MakeShared<FJsonValueNumber>(SourceIndex), MakeShared<FJsonValueNumber>(Index)};
						OutError = TypedNiagaraFailure(TEXT("graph_compound_niagara_module_not_unique"),
							TEXT("More than one module in the selected stage matches the reorder item reference."),
							Selector.GetTargetRef(), TEXT("stage.modules.matching_indices"),
							MakeShared<FJsonValueArray>(MoveTemp(MatchingIndices)), MakeShared<FJsonValueString>(TEXT("one matching module")));
						OutError.CandidateName = Selector.GetItemRef();
						return false;
					}
					Module = Modules[Index];
					SourceIndex = Index;
				}
			}
			if (!Module)
			{
				OutError = SelectionMatches(TEXT("graph_compound_niagara_module_missing"),
					TEXT("The reorder item module is absent from the selected stage."),
					Selector, TEXT("item_ref"), Selector.GetItemRef(), 0);
				return false;
			}
			int32 TargetIndex = 0;
			if (!Selector.GetPredecessorRef().IsEmpty())
			{
				FGuid PredecessorGuid;
				const bool bParsed = TryParseModuleGuid(Selector.GetPredecessorRef(), PredecessorGuid);
				if (!bParsed || PredecessorGuid == ModuleGuid)
				{
					OutError = TypedNiagaraFailure(TEXT("graph_compound_niagara_predecessor_invalid"),
						bParsed ? TEXT("A module cannot be its own reorder predecessor.")
							: TEXT("The predecessor reference must contain a valid, non-zero module GUID."),
						Selector.GetTargetRef(), TEXT("predecessor_ref"), MakeShared<FJsonValueString>(Selector.GetPredecessorRef()),
						MakeShared<FJsonValueString>(bParsed ? TEXT("a module different from item_ref") : TEXT("module:<non-zero GUID>")));
					OutError.CandidateName = Selector.GetItemRef();
					return false;
				}
				const int32 PredecessorIndex = Modules.IndexOfByPredicate(
					[PredecessorGuid](const UNiagaraNodeFunctionCall* Candidate)
					{
						return Candidate && Candidate->NodeGuid == PredecessorGuid;
					});
				if (PredecessorIndex == INDEX_NONE)
				{
					OutError = SelectionMatches(TEXT("graph_compound_niagara_predecessor_missing"),
						TEXT("The reorder predecessor module is absent from the selected stage."),
						Selector, TEXT("predecessor_ref"), Selector.GetPredecessorRef(), 0);
					return false;
				}
				TargetIndex = SourceIndex < PredecessorIndex
					? PredecessorIndex : PredecessorIndex + 1;
			}
			int32 NewIndex = INDEX_NONE;
			FMaterialNiagaraGraphNativeMutationReceipt Receipt;
			FMaterialNiagaraGraphAdapterError Error;
			if (!NativeHook.TryMoveNiagaraModule(
					System, Stage.EmitterHandleId, Stage.Usage, Stage.UsageId,
					*Module, TargetIndex, NewIndex, Receipt, Error))
			{
				SetError(OutError, Error, Selector.GetItemRef());
				return false;
			}
			return true;
		}

		bool TryCreateExecutionContext(
			const FUeAgentDescriptorIdentity& DescriptorIdentity,
			FUeAgentExecutionContextRefPtr& OutContext,
			FGraphCompoundRuntimeError& OutError)
		{
			FUeAgentExecutionContextInput Input;
			Input.ProjectInstanceId = TEXT("graph-compound-project");
			Input.EditorInstanceId = TEXT("graph-compound-editor");
			Input.StatePlane = EUeAgentStatePlane::Persisted;
			Input.ContextRevision = TEXT("graph-compound-niagara-typed-v1");
			FUeAgentExecutionContextError Error;
			if (!FUeAgentExecutionContextRef::TryCreate(
					Input, DescriptorIdentity, EUeAgentRequestKind::Operation,
					FUeAgentExecutionContextPolicy(
						{EUeAgentStatePlane::Persisted}, {},
						EUeAgentContextResolutionPolicy::ExplicitOnly,
						EUeAgentTransientContextPolicy::ReadOnly),
					OutContext, Error))
			{
				MarkGraphCompoundNativeDiagnostic(Error);
				SetError(OutError, Error, DescriptorIdentity.GetDescriptorId());
				return false;
			}
			return true;
		}

		struct FCompoundApplyContext final
		{
			TSharedPtr<FUeAgentNiagaraCompoundNativeHook> Native;
			TSharedPtr<FUeAgentNiagaraCompoundEffectHook> Effect;
			TOptional<FExecutionMutationRequest> Request;
			FUeAgentGenericEffectApplyResult Result;
			FString OperationId;
			bool bApplied = false;
		};

		bool TryApplyCompound(
			const FGraphCompoundDomainRouteProfile& Profile,
			const FGraphCompoundNiagaraMutationSelector& Selector,
			const FPlanningPreflightPlanRef& OuterPlan,
			const FUeAgentCanonicalValueRef& After,
			const int64 FencingToken,
			TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources,
			FCompoundApplyContext& OutContext,
			FGraphCompoundRuntimeError& OutError)
		{
			const auto Clock = []() { return FPlatformTime::Seconds(); };
			FUeAgentCancellationSource Cancellation(Clock);
			const FUeAgentOperationDeadline Deadline;
			TSharedPtr<const FUeAgentCandidateBinding> Binding;
			if (!TryFindCompoundBinding(
					Profile, Selector, *Resources, Deadline,
					Cancellation.GetToken(), Binding, OutError))
			{
				return false;
			}
			OutContext.Native =
				MakeShared<FUeAgentNiagaraCompoundNativeHook>(
					NiagaraCompoundGeneration);
			const FUeAgentStateReadRequest ReadRequest(
				*Binding, EUeAgentStatePlane::Persisted);
			TSharedPtr<const FUeAgentUeReadCapture> BeforeCapture;
			FUeAgentGenericReadError ReadError;
			const bool bCaptured = OutContext.Native->TryCapture(
					ReadRequest, *Resources, Deadline,
					Cancellation.GetToken(), BeforeCapture, ReadError);
			if (!bCaptured
				|| !BeforeCapture.IsValid()
				|| BeforeCapture->GetEntries().Num() != 1)
			{
				OutError = CompoundCaptureFailure(BeforeCapture, MoveTemp(ReadError), *Binding, bCaptured);
				return false;
			}
			const FUeAgentCanonicalIdentity& CandidateIdentity =
				Binding->GetSummary().GetIdentity();
			TArray<FPlannedChange> Changes;
			Changes.Emplace(
				CandidateIdentity, EChangeIntent::Update,
				BeforeCapture->GetEntries()[0].GetValue(), After,
				OuterPlan->GetAuthorityScopes(),
				TEXT("graph-compound:niagara-compound-direct"));
			TArray<FImpact> Impacts;
			Impacts.Emplace(
				CandidateIdentity, CandidateIdentity, TEXT("direct"),
				TEXT("graph-compound:niagara-compound-direct"));
			const FPlanningPreflightPlanRef InnerPlan =
				MakeShared<FPlanningPreflightPlan>(
					Profile.GetExecutionContract().GetDescriptorIdentity(),
					OuterPlan->GetRequestId() + TEXT(":compound"),
					OuterPlan->GetIdempotencyKey() + TEXT(":compound"),
					OuterPlan->GetCallerId(),
					OuterPlan->GetAuthorityScopes(),
					BeforeCapture->GetSnapshotId(),
					BeforeCapture->GetRevision(),
					BeforeCapture->GetEvidenceId(),
					FChangeSet(MoveTemp(Changes)),
					FImpactSet(MoveTemp(Impacts)),
					TArray<FPrecondition>(),
					Json::Sha256Utf8(
						OuterPlan->GetFingerprint() + TEXT(":")
						+ CandidateIdentity.GetCanonicalKey()));
			FUeAgentExecutionContextRefPtr ExecutionContext;
			if (!TryCreateExecutionContext(
					InnerPlan->GetDescriptorIdentity(),
					ExecutionContext, OutError))
			{
				return false;
			}
			OutContext.OperationId =
				TEXT("graph-compound:") + OuterPlan->GetRequestId();
			OutContext.Request.Emplace(
				OutContext.OperationId,
				TEXT("graph-compound-niagara-typed"),
				InnerPlan, ExecutionContext.ToSharedRef(), Resources,
				Deadline, Cancellation.GetToken(),
				EExecutionThreadAffinity::GameThread,
				EExecutionReentrancyPolicy::RejectSameTarget);
			OutContext.Effect = MakeShared<FUeAgentNiagaraCompoundEffectHook>(
				OutContext.Native.ToSharedRef());
			const FUeAgentGenericEffectBinding EffectBinding(
				*Binding, TEXT("UE.NiagaraInput:compound"),
				FUeAgentCanonicalType(
					TEXT("uai.niagara.input.v1"),
					EUeAgentValueKind::Object, false, TEXT("object")),
				TEXT("graph-compound-niagara-native-input"));
			FExecutionMutationError Error;
			const bool bApplied = OutContext.Effect->TryApply(
					OutContext.Request.GetValue(),
					FLockFenceLease(
						TEXT("graph-compound-niagara-typed"),
						{CandidateIdentity.GetCanonicalKey()}, FencingToken),
					{EffectBinding}, *Resources, OutContext.Result, Error);
			if (!bApplied || !OutContext.Result.IsValid())
			{
				OutError = Error;
				if (OutError.IsEmpty())
				{
					if (bApplied)
					{
						OutError = OutContext.Result.GetValidationDiagnostic();
						MarkGraphCompoundNativeDiagnostic(OutError);
					}
					else
					{
						OutError = TypedNiagaraFailure(TEXT("graph_compound_niagara_compound_apply_failed"),
							TEXT("The Niagara compound effect failed without supplying a diagnostic."),
							Selector.GetTargetRef(), TEXT("effect.apply.succeeded"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
						OutError.MutationMayHaveStarted = true;
					}
				}
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Selector.GetTargetRef();
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Selector.GetStableSemanticId();
				return false;
			}
			OutContext.bApplied = true;
			return true;
		}

		void TryRollbackCompound(
			FCompoundApplyContext& Context,
			FUeAgentResourceTracker& Resources,
			FGraphCompoundRuntimeError& OutRecovery)
		{
			OutRecovery.Reset();
			if (!Context.bApplied || !Context.Effect.IsValid() || !Context.Request.IsSet()) return;
			bool bConfirmed = false;
			const bool bRestored = Context.Effect->TryRollback(
				Context.Request.GetValue(), Context.Result, Resources, bConfirmed, OutRecovery);
			if (!bRestored || !bConfirmed)
			{
				if (OutRecovery.IsEmpty())
				{
					OutRecovery = TypedNiagaraFailure(TEXT("graph_compound_niagara_compound_rollback_failed"),
						bRestored ? TEXT("The Niagara compound rollback did not confirm restoration.")
							: TEXT("The Niagara compound rollback failed without supplying a diagnostic."),
						FString(), bRestored ? TEXT("rollback.confirmed") : TEXT("rollback.succeeded"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					OutRecovery.Stage = TEXT("rollback");
				}
				if (OutRecovery.Stage.IsEmpty()) OutRecovery.Stage = TEXT("rollback");
				OutRecovery.RollbackAttempted = true;
				OutRecovery.RollbackSucceeded = false;
				OutRecovery.MutationMayHaveStarted = true;
			}
			Context.bApplied = false;
		}

		bool TryApplyRenderer(
			UNiagaraSystem& System,
			const FGraphCompoundNiagaraMutationSelector& Selector,
			const FUeAgentCanonicalValueRef& After,
			FGraphCompoundRuntimeError& OutError)
		{
			if (After->GetKind() != EUeAgentValueKind::String
				|| After->GetText().IsEmpty())
			{
				OutError = After->GetKind() != EUeAgentValueKind::String
					? TypedNiagaraFailure(TEXT("graph_compound_niagara_renderer_value_invalid"),
						TEXT("The renderer attribute value must be a string."), Selector.GetTargetRef(), TEXT("after.kind"),
						MakeShared<FJsonValueNumber>(static_cast<int32>(After->GetKind())),
						MakeShared<FJsonValueNumber>(static_cast<int32>(EUeAgentValueKind::String)))
					: TypedNiagaraFailure(TEXT("graph_compound_niagara_renderer_value_invalid"),
						TEXT("The renderer attribute name must not be empty."), Selector.GetTargetRef(), TEXT("after"),
						MakeShared<FJsonValueString>(After->GetText()), MakeShared<FJsonValueString>(TEXT("non-empty attribute name")));
				OutError.CandidateName = Selector.GetParameterName();
				return false;
			}
			UNiagaraRendererProperties* Renderer = nullptr;
			FStructProperty* Property = nullptr;
			FNiagaraVariableAttributeBinding* Binding = nullptr;
			FVersionedNiagaraEmitter Emitter;
			if (!TryResolveRendererBinding(
					System, Selector, Renderer, Property, Binding,
					Emitter, OutError))
			{
				return false;
			}
			Renderer->PreEditChange(Property);
			Binding->SetValue(
				FName(*After->GetText()), Emitter,
				Renderer->GetCurrentSourceMode());
			FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
			Renderer->PostEditChangeProperty(Event);
			return true;
		}

		bool TryApplyMutation(
			const FGraphCompoundDomainRouteProfile& Profile,
			UNiagaraSystem& System,
			const FGraphCompoundNiagaraMutationSelector& Selector,
			const FPlanningPreflightPlanRef& Plan,
			const FUeAgentCanonicalValueRef& After,
			const int64 FencingToken,
			FMaterialNiagaraGraphNativeHook& NativeHook,
			TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources,
			FCompoundApplyContext& OutCompoundContext,
			FGraphCompoundRuntimeError& OutError)
		{
			switch (Selector.GetInputKind())
			{
			case EGraphCompoundTypedInputKind::NiagaraStackReorder:
				if (After->GetKind() != EUeAgentValueKind::String)
				{
					OutError = TypedNiagaraFailure(TEXT("graph_compound_niagara_reorder_after_invalid"),
						TEXT("The reorder proposed value must be a predecessor-reference string."), Selector.GetTargetRef(), TEXT("after.kind"),
						MakeShared<FJsonValueNumber>(static_cast<int32>(After->GetKind())),
						MakeShared<FJsonValueNumber>(static_cast<int32>(EUeAgentValueKind::String)));
					return false;
				}
				if (After->GetText() != Selector.GetPredecessorRef())
				{
					OutError = TypedNiagaraFailure(TEXT("graph_compound_niagara_reorder_after_invalid"),
						TEXT("The reorder proposed value differs from predecessor_ref."), Selector.GetTargetRef(), TEXT("after"),
						MakeShared<FJsonValueString>(After->GetText()), MakeShared<FJsonValueString>(Selector.GetPredecessorRef()));
					return false;
				}
				return TryApplyReorder(System, Selector, NativeHook, OutError);
			case EGraphCompoundTypedInputKind::NiagaraStackIssueFix:
			{
				if (After->GetKind() != EUeAgentValueKind::String
					|| After->GetText() != TEXT("resolved"))
				{
					OutError = After->GetKind() != EUeAgentValueKind::String
						? TypedNiagaraFailure(TEXT("graph_compound_niagara_issue_after_invalid"),
							TEXT("The issue-fix proposed value must be a string."), Selector.GetTargetRef(), TEXT("after.kind"),
							MakeShared<FJsonValueNumber>(static_cast<int32>(After->GetKind())),
							MakeShared<FJsonValueNumber>(static_cast<int32>(EUeAgentValueKind::String)))
						: TypedNiagaraFailure(TEXT("graph_compound_niagara_issue_after_invalid"),
							TEXT("The issue-fix proposed value must be resolved."), Selector.GetTargetRef(), TEXT("after"),
							MakeShared<FJsonValueString>(After->GetText()), MakeShared<FJsonValueString>(TEXT("resolved")));
					OutError.CandidateName = Selector.GetIssueId();
					return false;
				}
				FString State;
				if (!TryIssueState(System, Selector, true, State, OutError)) return false;
				if (State != TEXT("unresolved"))
				{
					OutError = TypedNiagaraFailure(TEXT("graph_compound_niagara_issue_already_resolved"),
						TEXT("The issue is no longer unresolved, so no fix was executed."), Selector.GetTargetRef(), TEXT("issue.state"),
						MakeShared<FJsonValueString>(State), MakeShared<FJsonValueString>(TEXT("unresolved")));
					OutError.CandidateName = Selector.GetIssueId();
					return false;
				}
				return true;
			}
			case EGraphCompoundTypedInputKind::NiagaraCompoundInput:
				return TryApplyCompound(
					Profile, Selector, Plan, After, FencingToken,
					Resources, OutCompoundContext, OutError);
			case EGraphCompoundTypedInputKind::NiagaraRendererEventParameter:
				return TryApplyRenderer(System, Selector, After, OutError);
			default:
				OutError = TypedNiagaraFailure(TEXT("graph_compound_niagara_selector_kind_invalid"),
					TEXT("This Niagara selector kind is not supported by typed mutation."), Selector.GetTargetRef(), TEXT("input_kind"),
					MakeShared<FJsonValueNumber>(static_cast<int32>(Selector.GetInputKind())),
					MakeShared<FJsonValueString>(TEXT("NiagaraStackReorder, NiagaraStackIssueFix, NiagaraCompoundInput or NiagaraRendererEventParameter")));
				return false;
			}
		}

		FAssetDurableSessionRef PersistedSession(
			const FAssetDurableSessionRef& Prepared,
			const FDurableReceipt& Receipt)
		{
			return MakeShared<const FAssetDurableSession>(
				Prepared->GetRequest(), Prepared->GetInventory(),
				Prepared->GetJournalId(), Prepared->GetJournalPath(),
				Prepared->GetJournalRoot(), Prepared->GetTransactionRoot(),
				Prepared->GetStagingRoot(), Receipt.GetPhaseHistory());
		}

		bool TryRollbackDurable(
			const FString& TargetRef,
			const FAssetDurableSessionRef& Session,
			FPackageAssetDurableBackend& Backend,
			const int64 FencingToken,
			FUeAgentResourceTracker& Resources,
			UE::UAI::Contract::FDiagnostic& OutDetail)
		{
			return UeAgentAuthoringOperationApply::TryRestorePackageDurability(
				TargetRef, false, Session, Backend, FencingToken,
				Resources, OutDetail);
		}

		bool TryValidateCompileReadback(
			UNiagaraSystem& System,
			UE::UAI::Contract::FDiagnostic& OutDiagnostic)
		{
			const UeAgentNiagaraCompileValidation::FResult Compile =
				UeAgentNiagaraCompileValidation::InspectAsset(&System);
			if (!Compile.bCompilationComplete || Compile.bHasError || Compile.ScriptCount <= 0)
			{
				OutDiagnostic = Compile.GetFailure(System.GetPathName());
				if (OutDiagnostic.IsEmpty())
					OutDiagnostic = TypedNiagaraFailure(TEXT("graph_compound_niagara_compile_readback_failed"),
						TEXT("Niagara compilation produced no scripts for independent readback."), System.GetPathName(),
						TEXT("script_count"), MakeShared<FJsonValueNumber>(Compile.ScriptCount),
						MakeShared<FJsonValueString>(TEXT("positive script count")));
				return false;
			}
			const bool bOutstanding = System.HasOutstandingCompilationRequests(true);
			const bool bNeedsCompile = !bOutstanding && System.NeedsRequestCompile();
			if (bOutstanding || bNeedsCompile)
			{
				OutDiagnostic = TypedNiagaraFailure(TEXT("graph_compound_niagara_compile_readback_failed"),
					bOutstanding ? TEXT("The Niagara system still has outstanding compilation requests.")
						: TEXT("The Niagara system still requires a compile request."),
					System.GetPathName(), bOutstanding ? TEXT("compilation.outstanding_requests") : TEXT("compilation.needs_request"),
					MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
				return false;
			}
			return true;
		}

		bool TrySerializeResult(
			const FString& TargetRef,
			FUeAgentContractValue& OutResult,
			FGraphCompoundRuntimeError& OutError)
		{
			const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("target_ref"), TargetRef);
			FString Canonical;
			UE::UAI::Contract::FDiagnostic Error;
			if (!Json::TryCanonicalize(Root, Canonical, Error))
			{
				MarkGraphCompoundNativeDiagnostic(Error);
				if (Error.TargetPath.IsEmpty()) Error.TargetPath = TargetRef;
				SetError(OutError, Error, TargetRef);
				return false;
			}
			OutResult = FUeAgentContractValue(MoveTemp(Canonical));
			return true;
		}

		class FNiagaraTypedReadbackOracle final
			: public IIndependentReadbackOracle
		{
		public:
			FNiagaraTypedReadbackOracle(
				FGraphCompoundDomainRouteProfile InProfile,
				FGraphCompoundNiagaraMutationSelector InSelector,
				FMaterialNiagaraGraphNativeHook& InNativeHook)
				: Profile(MoveTemp(InProfile))
				, Selector(MoveTemp(InSelector))
				, NativeHook(InNativeHook)
			{
			}

			const FString& GetOracleId() const override
			{
				return NiagaraTypedReadbackOracleId;
			}

			bool TryRead(
				const FIndependentReadbackRequest& Request,
				FUeAgentResourceTracker& Resources,
				FIndependentReadbackObservation& OutObservation,
				FVerificationError& OutError) override
			{
				OutObservation = FIndependentReadbackObservation();
				OutError = FVerificationError();
				UE::UAI::Contract::FDiagnostic Detail;
				if (!UeAgentAuthoringOperationApply::TryReloadPersistedPackage(Selector.GetTargetRef(), Detail))
				{
					OutError = Detail.IsEmpty() ? TypedNiagaraFailure(TEXT("graph_compound_niagara_unload_failed"),
						TEXT("The persisted Niagara package could not be reloaded for independent readback."),
						Selector.GetTargetRef(), TEXT("package.reload.succeeded"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)) : Detail;
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Selector.GetTargetRef();
					return false;
				}
				FGraphCompoundRuntimeError ResolveError;
				if (!TryResolveSystem(Selector.GetTargetRef(), ResolveError))
				{
					OutError = ResolveError;
					return false;
				}
				FUeAgentCanonicalValuePtr Actual;
				FGraphCompoundRuntimeError Error;
				if (!TryCaptureCurrentValue(
						Profile, Selector, NativeHook, Resources,
						Actual, Error))
				{
					OutObservation = FIndependentReadbackObservation::Unavailable(Error, Error.SubjectId);
					return true;
				}
				UNiagaraSystem* const System =
					TryResolveSystem(Selector.GetTargetRef(), Error);
				UE::UAI::Contract::FDiagnostic Diagnostic;
				const bool bDirty = System && System->GetOutermost()->IsDirty();
				if (bDirty)
					Diagnostic = TypedNiagaraFailure(TEXT("graph_compound_niagara_readback_package_dirty"),
						TEXT("The reloaded Niagara package is still dirty."), Selector.GetTargetRef(),
						TEXT("package.dirty"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
				if (!System || bDirty || !TryValidateCompileReadback(*System, Diagnostic))
				{
					OutObservation = FIndependentReadbackObservation::Unavailable(!System ? Error : Diagnostic,
						TEXT("reload:") + Selector.GetTargetRef());
					return true;
				}
				OutObservation = FIndependentReadbackObservation::Available(
					Actual.ToSharedRef(),
					TEXT("reload:") + Request.GetSubjectRef() + TEXT(":")
						+ FGuid::NewGuid().ToString(EGuidFormats::Digits));
				return true;
			}

		private:
			FGraphCompoundDomainRouteProfile Profile;
			FGraphCompoundNiagaraMutationSelector Selector;
			FMaterialNiagaraGraphNativeHook& NativeHook;
		};
	}

	FGraphCompoundNiagaraTypedMutationProduction::
	FGraphCompoundNiagaraTypedMutationProduction(
		FString InJournalRoot,
		FMaterialNiagaraGraphNativeHook& InNativeHook,
		FPackageAssetDurableBackend& InDurableBackend,
		int64& InLastFencingToken)
		: JournalRoot(MoveTemp(InJournalRoot))
		, NativeHook(InNativeHook)
		, DurableBackend(InDurableBackend)
		, LastFencingToken(InLastFencingToken)
	{
	}

	bool FGraphCompoundNiagaraTypedMutationProduction::TryCaptureSnapshot(
		const FGraphCompoundDomainRouteProfile& Profile,
		const FGraphCompoundNiagaraMutationSelector& Selector,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalSnapshotPtr& OutSnapshot,
		FGraphCompoundRuntimeError& OutError) const
	{
		OutSnapshot.Reset();
		OutError = {};
		if (!IsInGameThread() || !Selector.IsValid()
			|| Profile.GetTypedInputKind() != Selector.GetInputKind())
		{
			if (!IsInGameThread())
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_niagara_typed_snapshot_thread_invalid"),
					TEXT("Niagara mutation snapshot capture requires the game thread."), TEXT("graph_compound"), FString(), Profile.GetCapabilityId());
				OutError.SourceKind = TEXT("native_result");
				OutError.PropertyPath = TEXT("is_game_thread");
				OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
				OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			}
			else if (!Selector.IsValid())
			{
				OutError = Selector.GetValidationDiagnostic();
			}
			else
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_niagara_typed_selector_invalid"),
					TEXT("The Niagara selector input kind does not match its route profile."), TEXT("graph_compound"), FString(), Profile.GetCapabilityId());
				OutError.SourceKind = TEXT("native_result");
				OutError.PropertyPath = TEXT("selector.input_kind");
				OutError.ActualValue = MakeShared<FJsonValueNumber>(static_cast<int32>(Selector.GetInputKind()));
				OutError.ExpectedValue = MakeShared<FJsonValueNumber>(static_cast<int32>(Profile.GetTypedInputKind()));
			}
			return false;
		}
		FUeAgentCanonicalValuePtr Current;
		if (!TryCaptureCurrentValue(
				Profile, Selector, NativeHook, Resources,
				Current, OutError))
		{
			return false;
		}
		if (Selector.GetInputKind()
				== EGraphCompoundTypedInputKind::NiagaraStackIssueFix
			&& (Current->GetKind() != EUeAgentValueKind::String
				|| Current->GetText() != TEXT("unresolved")))
		{
			OutError = Current->GetKind() != EUeAgentValueKind::String
				? TypedNiagaraFailure(TEXT("graph_compound_niagara_issue_already_resolved"),
					TEXT("The captured stack issue state is not a string."), Selector.GetTargetRef(), TEXT("issue.state.kind"),
					MakeShared<FJsonValueNumber>(static_cast<int32>(Current->GetKind())),
					MakeShared<FJsonValueNumber>(static_cast<int32>(EUeAgentValueKind::String)))
				: TypedNiagaraFailure(TEXT("graph_compound_niagara_issue_already_resolved"),
					TEXT("The selected stack issue is no longer unresolved."), Selector.GetTargetRef(), TEXT("issue.state"),
					MakeShared<FJsonValueString>(Current->GetText()), MakeShared<FJsonValueString>(TEXT("unresolved")));
			OutError.CandidateName = Selector.GetIssueId();
			return false;
		}
		return TryBuildSnapshot(
			Profile, Selector, Current.ToSharedRef(),
			Resources, OutSnapshot, OutError);
	}

	bool FGraphCompoundNiagaraTypedMutationProduction::TryExecute(
		const FGraphCompoundDomainRouteProfile& Profile,
		const FGraphCompoundNiagaraMutationSelector& Selector,
		const FGraphCompoundCanonicalPayload& Payload,
		const FPlanningPreflightPlanRef& Plan,
		const FUeAgentCallerContext& CallerContext,
		TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources,
		const TSharedRef<IUeAgentCancellationSignal>& Cancellation,
		FUeAgentContractValue& OutResult,
		FGraphCompoundRuntimeError& OutError)
	{
		OutResult = FUeAgentContractValue(TEXT("{}"));
		OutError = {};
		const bool bPayloadValid = Selector.IsValid()
			&& Profile.GetTypedInputKind() == Selector.GetInputKind()
			&& Payload.GetEdits().Num() == 1
			&& Payload.GetEdits()[0].GetIdentity().GetSemanticId()
				== Selector.GetStableSemanticId()
			&& Payload.GetEdits()[0].GetIdentity().GetOwnerId()
				== Selector.GetTargetRef()
			&& Payload.GetEdits()[0].GetProposedAfter().IsValid()
			&& Plan->GetDescriptorIdentity()
				== Profile.GetExecutionContract().GetDescriptorIdentity()
			&& Plan->GetChangeSet().GetChanges().Num() == 1
			&& Plan->GetChangeSet().GetChanges()[0].GetIdentity()
				.GetSemanticId() == Selector.GetStableSemanticId();
		if (!IsInGameThread() || Cancellation->IsCancelled()
			|| !bPayloadValid)
		{
			if (Cancellation->IsCancelled())
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_niagara_typed_cancelled"),
					TEXT("Niagara typed mutation was cancelled before execution."), TEXT("graph_compound"), FString(), Selector.GetTargetRef());
				OutError.SourceKind = TEXT("native_result");
				OutError.PropertyPath = TEXT("cancellation_requested");
				OutError.ActualValue = MakeShared<FJsonValueBoolean>(true);
				OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(false);
			}
			else if (!IsInGameThread())
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_niagara_typed_thread_invalid"),
					TEXT("Niagara typed mutation requires the game thread."), TEXT("graph_compound"), FString(), Selector.GetTargetRef());
				OutError.SourceKind = TEXT("native_result");
				OutError.PropertyPath = TEXT("is_game_thread");
				OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
				OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			}
			else if (!Selector.IsValid())
			{
				OutError = Selector.GetValidationDiagnostic();
			}
			else
			{
				auto Fail = [&](const FString& Reason, const FString& Field, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
				{
					OutError = TypedNiagaraFailure(TEXT("graph_compound_niagara_typed_plan_invalid"),
						Reason, Selector.GetTargetRef(), Field, MoveTemp(Actual), MoveTemp(Expected));
					return false;
				};
				if (Profile.GetTypedInputKind() != Selector.GetInputKind())
					return Fail(TEXT("The Niagara selector input kind does not match its route profile."), TEXT("selector.input_kind"),
						MakeShared<FJsonValueNumber>(static_cast<int32>(Selector.GetInputKind())),
						MakeShared<FJsonValueNumber>(static_cast<int32>(Profile.GetTypedInputKind())));
				if (Payload.GetEdits().Num() != 1)
					return Fail(TEXT("Niagara typed mutation requires exactly one payload edit."), TEXT("edits.count"),
						MakeShared<FJsonValueNumber>(Payload.GetEdits().Num()), MakeShared<FJsonValueNumber>(1));
				const auto& Edit = Payload.GetEdits()[0];
				if (Edit.GetIdentity().GetSemanticId() != Selector.GetStableSemanticId())
				{
					Fail(TEXT("The payload edit does not address the selected Niagara mutation."), TEXT("edits[0].semantic_id"), nullptr, nullptr);
					OutError.ActualEvidence = MakeShared<FJsonValueString>(Edit.GetIdentity().GetSemanticId());
					OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Selector.GetStableSemanticId());
					return false;
				}
				if (Edit.GetIdentity().GetOwnerId() != Selector.GetTargetRef())
					return Fail(TEXT("The payload edit owner differs from the Niagara target."), TEXT("edits[0].owner"),
						MakeShared<FJsonValueString>(Edit.GetIdentity().GetOwnerId()), MakeShared<FJsonValueString>(Selector.GetTargetRef()));
				if (!Edit.GetProposedAfter().IsValid())
					return Fail(TEXT("The Niagara mutation edit has no proposed value."), TEXT("edits[0].after.present"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				const auto& ActualDescriptor = Plan->GetDescriptorIdentity();
				const auto& ExpectedDescriptor = Profile.GetExecutionContract().GetDescriptorIdentity();
				if (ActualDescriptor != ExpectedDescriptor)
				{
					if (ActualDescriptor.GetDescriptorId() != ExpectedDescriptor.GetDescriptorId())
						return Fail(TEXT("The Niagara plan descriptor id does not match the route descriptor."), TEXT("descriptor_identity.descriptor_id"),
							MakeShared<FJsonValueString>(ActualDescriptor.GetDescriptorId()), MakeShared<FJsonValueString>(ExpectedDescriptor.GetDescriptorId()));
					if (ActualDescriptor.GetVersion() != ExpectedDescriptor.GetVersion())
						return Fail(TEXT("The Niagara plan descriptor version does not match the route descriptor."), TEXT("descriptor_identity.version"),
							MakeShared<FJsonValueNumber>(ActualDescriptor.GetVersion()), MakeShared<FJsonValueNumber>(ExpectedDescriptor.GetVersion()));
					Fail(TEXT("The Niagara plan descriptor hash does not match the route descriptor."), TEXT("descriptor_identity.descriptor_hash"), nullptr, nullptr);
					OutError.ActualEvidence = MakeShared<FJsonValueString>(ActualDescriptor.GetDescriptorHash());
					OutError.ExpectedEvidence = MakeShared<FJsonValueString>(ExpectedDescriptor.GetDescriptorHash());
					return false;
				}
				if (Plan->GetChangeSet().GetChanges().Num() != 1)
					return Fail(TEXT("Niagara typed mutation requires exactly one planned change."), TEXT("changes.count"),
						MakeShared<FJsonValueNumber>(Plan->GetChangeSet().GetChanges().Num()), MakeShared<FJsonValueNumber>(1));
				Fail(TEXT("The planned change does not address the selected Niagara mutation."), TEXT("changes[0].semantic_id"), nullptr, nullptr);
				OutError.ActualEvidence = MakeShared<FJsonValueString>(Plan->GetChangeSet().GetChanges()[0].GetIdentity().GetSemanticId());
				OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Selector.GetStableSemanticId());
			}
			return false;
		}

		FUeAgentCanonicalSnapshotPtr Current;
		if (!TryCaptureSnapshot(
				Profile, Selector, *Resources, Current, OutError))
		{
			return false;
		}
		FPlanningPreflightError RevalidateError;
		if (!TryRevalidatePlan(
				Plan, Current.ToSharedRef(), CallerContext,
				*Resources, RevalidateError))
		{
			OutError = RevalidateError;
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = RevalidateError.SubjectId;
			return false;
		}

		UNiagaraSystem* System =
			TryResolveSystem(Selector.GetTargetRef(), OutError);
		if (!System)
		{
			return false;
		}
		const int64 FencingToken = ++LastFencingToken;
		const FString JournalOwner =
			TEXT("graph-compound:") + Plan->GetRequestId();
		const FString PackageName = PackageNameFromRef(Selector.GetTargetRef());
		const FAssetDurableRequest DurableRequest(
			Plan,
			{FAssetDurableTarget(
				Plan->GetChangeSet().GetChanges()[0].GetIdentity(),
				PackageName, EPackageHeaderKind::Asset,
				EAssetDurableIntent::Save,
				ERollbackGrade::DurableBackup)},
			FencingToken, JournalOwner, EAtomicityMode::CompensatedSaga);
		FAssetDurableSessionPtr Prepared;
		FAssetDurableError DurableError;
		if (!TryPrepareAssetDurability(
				DurableRequest, DurableBackend, JournalRoot,
				*Resources, Prepared, DurableError))
		{
			OutError = DurableError;
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = PackageName;
			return false;
		}

		const FUeAgentCanonicalValueRef After =
			Payload.GetEdits()[0].GetProposedAfter().ToSharedRef();
		FCompoundApplyContext CompoundContext;
		if (!TryApplyMutation(
				Profile, *System, Selector, Plan, After, FencingToken,
				NativeHook, Resources, CompoundContext, OutError))
		{
			FGraphCompoundRuntimeError CompoundRecovery;
			TryRollbackCompound(CompoundContext, *Resources, CompoundRecovery);
			if (!CompoundRecovery.IsEmpty() && CompoundRecovery.TargetPath.IsEmpty()) CompoundRecovery.TargetPath = Selector.GetTargetRef();
			OutError.Append(CompoundRecovery);
			System = nullptr;
			UE::UAI::Contract::FDiagnostic RollbackDetail;
			TryRollbackDurable(
				PackageName, Prepared.ToSharedRef(), DurableBackend,
				FencingToken, *Resources, RollbackDetail);
			OutError.Append(RollbackDetail);
			return false;
		}

		FMaterialNiagaraGraphCompileSettleReceipt Settle;
		FMaterialNiagaraGraphAdapterError NativeError;
		const bool bCompiled = NativeHook.TryCompileAndSettleNiagara(*System, Settle, NativeError);
		if (!bCompiled
			|| !Settle.IsCompilationComplete()
			|| Settle.GetCompileErrorCount() != 0
			|| Settle.GetStackIssueCount() != 0)
		{
			if (NativeError.IsEmpty())
			{
				if (!bCompiled)
					NativeError = TypedNiagaraFailure(TEXT("graph_compound_niagara_compile_failed"),
						TEXT("Native Niagara compilation failed without supplying a diagnostic."), Selector.GetTargetRef(),
						TEXT("compilation.succeeded"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				else if (!Settle.IsCompilationComplete())
					NativeError = TypedNiagaraFailure(TEXT("graph_compound_niagara_compile_not_settled"),
						TEXT("Niagara compilation did not complete."), Selector.GetTargetRef(),
						TEXT("compilation.complete"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				else if (Settle.GetCompileErrorCount() != 0)
					NativeError = TypedNiagaraFailure(TEXT("graph_compound_niagara_compile_not_settled"),
						TEXT("Niagara compilation reported errors."), Selector.GetTargetRef(),
						TEXT("compile_error_count"), MakeShared<FJsonValueNumber>(Settle.GetCompileErrorCount()), MakeShared<FJsonValueNumber>(0));
				else
					NativeError = TypedNiagaraFailure(TEXT("graph_compound_niagara_compile_not_settled"),
						TEXT("Niagara stack validation reported blocking issues."), Selector.GetTargetRef(),
						TEXT("stack_issue_count"), MakeShared<FJsonValueNumber>(Settle.GetStackIssueCount()), MakeShared<FJsonValueNumber>(0));
			}
			NativeError.MutationMayHaveStarted = true;
			FGraphCompoundRuntimeError CompoundRecovery;
			TryRollbackCompound(CompoundContext, *Resources, CompoundRecovery);
			if (!CompoundRecovery.IsEmpty() && CompoundRecovery.TargetPath.IsEmpty()) CompoundRecovery.TargetPath = Selector.GetTargetRef();
			NativeError.Append(CompoundRecovery);
			System = nullptr;
			UE::UAI::Contract::FDiagnostic RollbackDetail;
			TryRollbackDurable(
				PackageName, Prepared.ToSharedRef(), DurableBackend,
				FencingToken, *Resources, RollbackDetail);
			if (NativeError.TargetPath.IsEmpty()) NativeError.TargetPath = PackageName;
			SetError(OutError, NativeError, PackageName, RollbackDetail);
			return false;
		}
		System->GetOutermost()->MarkPackageDirty();
		FDurableReceipt DurableReceipt;
		if (!TryPersistAssetDurabilityForVerification(
				Prepared.ToSharedRef(), DurableBackend, FencingToken,
				*Resources, DurableReceipt, DurableError))
		{
			FGraphCompoundRuntimeError CompoundRecovery;
			TryRollbackCompound(CompoundContext, *Resources, CompoundRecovery);
			if (!CompoundRecovery.IsEmpty() && CompoundRecovery.TargetPath.IsEmpty()) CompoundRecovery.TargetPath = Selector.GetTargetRef();
			DurableError.Append(CompoundRecovery);
			System = nullptr;
			UE::UAI::Contract::FDiagnostic RollbackDetail;
			TryRollbackDurable(
				PackageName, Prepared.ToSharedRef(), DurableBackend,
				FencingToken, *Resources, RollbackDetail);
			SetError(OutError, DurableError, PackageName, RollbackDetail);
			OutError.MutationMayHaveStarted = true;
			return false;
		}
		System->GetOutermost()->SetDirtyFlag(false);
		System = nullptr;

		const FAssetDurableSessionRef Persisted =
			PersistedSession(Prepared.ToSharedRef(), DurableReceipt);
		const TSharedRef<FNiagaraTypedReadbackOracle, ESPMode::ThreadSafe>
			Oracle = MakeShared<FNiagaraTypedReadbackOracle,
				ESPMode::ThreadSafe>(Profile, Selector, NativeHook);
		TSharedPtr<FIndependentReadbackService> Readback;
		FVerificationError VerifyError;
		FIndependentReadbackOutcome Outcome;
		if (!FIndependentReadbackService::TryCreate(
				{Oracle}, Readback, VerifyError)
			|| !Readback->TryObserve(
				FIndependentReadbackRequest(
					TEXT("graph-compound:") + Plan->GetRequestId(),
					NiagaraTypedReadbackOracleId,
					Selector.GetStableSemanticId(), After),
				*Resources, Outcome, VerifyError))
		{
			UE::UAI::Contract::FDiagnostic RollbackDetail;
			TryRollbackDurable(
				PackageName, Persisted, DurableBackend,
				FencingToken, *Resources, RollbackDetail);
			SetError(OutError, Outcome.GetFailureDiagnostic(VerifyError), Selector.GetStableSemanticId(), RollbackDetail);
			if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Selector.GetTargetRef();
			OutError.MutationMayHaveStarted = true;
			return false;
		}

		const FDurableVerificationAcknowledgement Acknowledgement(
			DurableReceipt.GetJournalId(), Plan->GetFingerprint(),
			Prepared->GetInventory().GetInventoryHash(),
			Outcome.GetEvidenceId(), JournalOwner, FencingToken, Outcome.GetStatus() == EVerificationStatus::Verified);
		if (!TryAcknowledgeAssetDurability(
				Persisted, Acknowledgement, FencingToken,
				*Resources, DurableError))
		{
			UE::UAI::Contract::FDiagnostic RollbackDetail;
			TryRollbackDurable(
				PackageName, Persisted, DurableBackend,
				FencingToken, *Resources, RollbackDetail);
			SetError(OutError, DurableError, PackageName, RollbackDetail);
			OutError.MutationMayHaveStarted = true;
			return false;
		}
		if (CompoundContext.Native.IsValid()
			&& !CompoundContext.OperationId.IsEmpty())
		{
			CompoundContext.Native->ReleaseVerifiedOperation(
				CompoundContext.OperationId);
		}
		return TrySerializeResult(
			PackageName, OutResult, OutError);
	}
}


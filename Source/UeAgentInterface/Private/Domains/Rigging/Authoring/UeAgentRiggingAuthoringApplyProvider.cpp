// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Rigging/Authoring/UeAgentRiggingAuthoringApplyProvider.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAssetLifecycle.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringOps.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringState.h"
#include "Domains/Rigging/UeAgentControlRigDomainBackend.h"
#include "ControlRigBlueprint.h"
#include "Engine/Blueprint.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "UObject/StrongObjectPtr.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticCompare.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringAssetRemovalStage.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringObjectApplyProvider.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentApplyWorkspaceContract.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringAssetWorkspaceApply.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringAssetWorkspaceInput.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceDependencyGraph.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceLoader.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"

#include "Domains/Rigging/Authoring/UeAgentRiggingAuthoringDocuments.h"
#include "Domains/Rigging/Authoring/UeAgentRiggingAuthoringState.h"

namespace UeAgentRiggingAuthoring
{
	namespace
	{
		using namespace UeAgentApplyInputContract;
		using namespace UeAgentAuthoringOperationApply;

		void SetFailure(FOperationFailure& OutFailure, const UE::UAI::Contract::FDiagnostic& Diagnostic) {
            OutFailure.Diagnostic = Diagnostic;
        }

        UE::UAI::Contract::FDiagnostic ProviderFailure(const TCHAR* Code, const FString& Reason,
            const FString& TargetPath, const FString& Property, const TSharedPtr<FJsonValue>& Actual,
            const TSharedPtr<FJsonValue>& Expected) {
            auto Diagnostic = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Property, Expected, Actual,
                TEXT("authoring_apply"), Reason);
            Diagnostic.TargetPath = TargetPath.IsEmpty() ? FString() : ToObjectPath(TargetPath);
            Diagnostic.SourceKind = TEXT("native_result");
            Diagnostic.Source = {};
            Diagnostic.ActualValuePresent = Actual.IsValid();
            Diagnostic.ExpectedValuePresent = Expected.IsValid();
            return Diagnostic;
        }

		bool ReadBool(const TSharedPtr<const FJsonObject>& Object, const TCHAR* Field, const bool DefaultValue)
		{
			bool Value = DefaultValue;
			if (Object.IsValid()) Object->TryGetBoolField(Field, Value);
			return Value;
		}

		TSet<FString> GetDocumentFields(const EAssetKind Kind)
		{
			switch (Kind)
			{
			case EAssetKind::ControlRig:
				return { TEXT("rig_type"), TEXT("preview_mesh"), TEXT("shape_libraries"),
					TEXT("hierarchy"), TEXT("graphs"), TEXT("variables") };
			case EAssetKind::IKRig:
				return { TEXT("preview_mesh"), TEXT("retarget_root"), TEXT("goals"),
					TEXT("solvers"), TEXT("solver_order"), TEXT("chains") };
			case EAssetKind::IKRetargeter:
				return { TEXT("rigs"), TEXT("preview_meshes"), TEXT("ops"),
					TEXT("op_order"), TEXT("mappings"), TEXT("poses") };
			default:
				return {};
			}
		}

		bool ExtractDocument(
			const EAssetKind Kind,
			const TSharedPtr<const FJsonObject>& Extension,
			TSharedPtr<FJsonObject>& OutDocument,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutDocument.Reset();
			OutError.Reset();
			if (!Extension.IsValid()) return true;
			const TSet<FString> DocumentFields = GetDocumentFields(Kind);
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Extension->Values)
			{
				if (Field.Key == TEXT("compile") || Field.Key == TEXT("save")) continue;
				if (!DocumentFields.Contains(Field.Key))
				{
					TArray<FString> Allowed = DocumentFields.Array();
                    Allowed.Append({TEXT("compile"), TEXT("save")});
                    Allowed.Sort();
                    const FString Pointer = TEXT("/provider_extension/") + Field.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
                    OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("rigging_provider_extension_unknown_field"), Pointer,
                        MakeShared<FJsonValueString>(TEXT("one of: ") + FString::Join(Allowed, TEXT(", "))), MakeShared<FJsonValueString>(Field.Key),
                        TEXT("authoring_apply"), TEXT("The Rigging provider does not accept this extension field."));
                    OutError.SourceKind = TEXT("authored_value");
                    OutError.Source.JsonPath = Pointer;
                    OutError.Source.InternalJsonPath = Pointer;
					return false;
				}
				if (!OutDocument.IsValid()) OutDocument = MakeShared<FJsonObject>();
				OutDocument->SetField(Field.Key, Field.Value);
			}
			return true;
		}

		bool DocumentsEqual(UObject& A, UObject& B, const FProfile& Profile, UE::UAI::Contract::FDiagnostic& OutReadError, const FString& TargetPath)
		{
			OutReadError.Reset();
			// No-op detection consumes the same native declarations and comparator
			// as final readback, including unordered graph relations.
			auto Capture = [&](UObject& Asset, UeAgentAuthoringState::FCaptureResult& Result, const TCHAR* Code)
			{
				TArray<UeAgentAuthoringState::FReadRequest> Requests;
				if (!BuildAssetSnapshotReadRequests(Profile, &Asset, TargetPath, false, Requests, OutReadError)
					|| !UeAgentAuthoringState::CaptureSnapshot(Requests, Result, nullptr, nullptr,
						UeAgentAuthoringState::ECaptureValueMode::Native))
				{
					if (OutReadError.IsEmpty()) OutReadError = Result.Diagnostic;
					OutReadError = UE::UAI::Contract::FDiagnostic::WrapFailure(Code, OutReadError, TEXT("noop_comparison"));
					return false;
				}
				return true;
			};
			UeAgentAuthoringState::FCaptureResult Existing, Proposed;
			if (!Capture(A, Existing, TEXT("rigging_noop_existing_document_read_failed"))
				|| !Capture(B, Proposed, TEXT("rigging_noop_proposed_document_read_failed"))) return false;
			UeAgentAuthoringSemanticCompare::FComparisonReport Comparison;
			if (!UeAgentAuthoringSemanticCompare::CompareSnapshots(Existing.Snapshot, Proposed.Snapshot, {}, Comparison))
			{
				OutReadError = Comparison.Diagnostic;
				return false;
			}
			return Comparison.bEquivalent;
		}

		UObject* DuplicateAsset(UObject* Source, UObject* Outer, const FName Name, const EObjectFlags Flags,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			if (auto* Blueprint = Cast<UBlueprint>(Source))
				return UeAgentBlueprintAssetLifecycle::DuplicateBlueprint(Blueprint, Outer, Name, false, &OutError);
			return Source && Outer ? StaticDuplicateObject(Source, Outer, Name, Flags) : nullptr;
		}

		UObject* PublishAsset(UObject* Source, const FString& PackagePath, UE::UAI::Contract::FDiagnostic& OutError)
		{
			UPackage* Package = CreatePackage(*PackagePath);
			if (auto* Blueprint = Cast<UBlueprint>(Source))
				return UeAgentBlueprintAssetLifecycle::PublishBlueprint(Blueprint, Package,
					FPackageName::GetLongPackageAssetName(PackagePath), OutError);
			UObject* Published = DuplicateAsset(
				Source, Package, FName(*FPackageName::GetLongPackageAssetName(PackagePath)), RF_AllFlags, OutError);
			if (Published)
			{
				Published->ClearFlags(RF_Transient);
				Published->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
			}
			return Published;
		}

		bool DeleteAsset(UObject* Asset, const FString& Target, UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutError.Reset();
			if (!Asset) {
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("rigging_delete_target_missing"),
					TEXT("No rigging asset object was supplied for deletion."), TEXT("authoring_apply"));
				OutError.SourceKind = TEXT("native_result"); OutError.TargetPath = Target; OutError.PropertyPath = TEXT("asset");
				OutError.ActualValuePresent = true; OutError.ActualValue = MakeShared<FJsonValueNull>();
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("rigging asset object"));
				return false;
			}
			TGuardValue<bool> UnattendedGuard(GIsRunningUnattendedScript, true);
			if (!ObjectTools::DeleteSingleObject(Asset, true)) {
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("rigging_delete_rejected"),
					TEXT("The editor rejected deletion of the rigging asset object."), TEXT("authoring_apply"),
					TEXT("DeleteSingleObject returned false with reference checking enabled; its bool API does not distinguish deletion-policy and reference-check rejection."));
				OutError.SourceKind = TEXT("native_result"); OutError.TargetPath = Target; OutError.PropertyPath = TEXT("deletion_succeeded");
				OutError.ActualValuePresent = true; OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
				OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				return false;
			}
			return true;
		}

		TSharedRef<FJsonObject> StatsToJson(const FMutationStats& Stats)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetNumberField(TEXT("hierarchy_elements"), Stats.HierarchyElements);
			Result->SetNumberField(TEXT("graph_nodes"), Stats.GraphNodes);
			Result->SetNumberField(TEXT("graph_links"), Stats.GraphLinks);
			Result->SetNumberField(TEXT("goals"), Stats.Goals);
			Result->SetNumberField(TEXT("solvers"), Stats.Solvers);
			Result->SetNumberField(TEXT("chains"), Stats.Chains);
			Result->SetNumberField(TEXT("retarget_ops"), Stats.RetargetOps);
			Result->SetNumberField(TEXT("mappings"), Stats.Mappings);
			Result->SetNumberField(TEXT("poses"), Stats.Poses);
			return Result;
		}

		FMutationStats StatsFromJson(const TSharedPtr<FJsonObject>& Object)
		{
			FMutationStats Result;
			if (!Object.IsValid()) return Result;
			Result.HierarchyElements = Object->GetIntegerField(TEXT("hierarchy_elements"));
			Result.GraphNodes = Object->GetIntegerField(TEXT("graph_nodes"));
			Result.GraphLinks = Object->GetIntegerField(TEXT("graph_links"));
			Result.Goals = Object->GetIntegerField(TEXT("goals"));
			Result.Solvers = Object->GetIntegerField(TEXT("solvers"));
			Result.Chains = Object->GetIntegerField(TEXT("chains"));
			Result.RetargetOps = Object->GetIntegerField(TEXT("retarget_ops"));
			Result.Mappings = Object->GetIntegerField(TEXT("mappings"));
			Result.Poses = Object->GetIntegerField(TEXT("poses"));
			return Result;
		}

		class FAssetLifecycleState final : public IReflectionOperationLifecycleState
		{
		public:
			FAssetLifecycleState(
				const FProfile& InProfile,
				FString InPackagePath,
				const EOperation InOperation,
				UObject* InExisting,
				UObject* InProposed,
				TSharedPtr<const FJsonObject> InDocument,
				const FMutationStats& InStats,
				const bool bInCompile,
				const bool bInSave,
				const bool bInDocumentNoOp,
				TSharedPtr<const FJsonObject> InCapturedDocument,
				const TArray<UBlueprint*>& InDependents)
				: Profile(InProfile)
				, PackagePath(MoveTemp(InPackagePath))
				, Operation(InOperation)
				, Existing(InExisting)
				, Proposed(InProposed)
				, Document(MoveTemp(InDocument))
				, Stats(InStats)
				, bCompile(bInCompile)
				, bSave(bInSave)
				, bDocumentNoOp(bInDocumentNoOp)
				, CapturedDocument(MoveTemp(InCapturedDocument))
			{
				for (UBlueprint* Child : InDependents)
				{
					auto& Entry = Dependents.AddDefaulted_GetRef();
					Entry.PackagePath = Child->GetOutermost()->GetName();
					Entry.Target.Reset(Child);
				}
			}

			virtual FString GetTargetKey() const override { return PackagePath; }
			virtual EOperation GetResolvedOperation() const override { return Operation; }
			virtual bool IsNoOp() const override
			{
				return Operation == EOperation::Update && bDocumentNoOp && !bCompile && !bSave;
			}
			virtual FReflectionRoot GetProposedRoot() const override
			{
				return FReflectionRoot::MakeObject(Proposed.Get());
			}
			virtual FReflectionRoot GetActualRoot() const override
			{
				return FReflectionRoot::MakeObject(Actual ? Actual : Existing);
			}
			virtual bool ShouldApplyPropertiesToActualRoot() const override
			{
				return Operation == EOperation::Update;
			}

			virtual bool CaptureRollback(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				if (Operation == EOperation::Update && Existing && Profile.Kind != EAssetKind::ControlRig)
				{
					UE::UAI::Contract::FDiagnostic Error;
					if (!BuildDocument(Profile.Kind, *Existing, RollbackDocument, Error, PackagePath))
					{
						SetFailure(OutFailure, Error);
						return false;
					}
				}
				else if (Operation == EOperation::Remove && Existing)
				{
					UE::UAI::Contract::FDiagnostic Error;
					if (!RemovalStage.Capture(Existing, Error))
					{
						SetFailure(OutFailure, Error);
						return false;
					}
				}
				return true;
			}

			virtual bool ApplyProposed(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				if (Operation == EOperation::Update && Profile.Kind == EAssetKind::ControlRig)
				{
					// The independently prepared document remains the root expectation.
					// Capture native dependent results only after their reconstruction,
					// as the shared Blueprint lifecycle does for its loaded children.
					return ApplyUpdateDocument(OutFailure);
				}
				return true;
			}

			virtual bool ApplyLifecycle(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				if (Operation == EOperation::Remove)
				{
					UE::UAI::Contract::FDiagnostic Error;
					if (!RemovalStage.Stage(Error))
					{
						SetFailure(OutFailure, Error);
						return false;
					}
					Actual = nullptr;
					return true;
				}
				if (Operation == EOperation::Update)
				{
					if (Profile.Kind != EAssetKind::ControlRig && !ApplyUpdateDocument(OutFailure)) return false;
					Actual = Existing;
					return true;
				}
				if (!Proposed.IsValid())
				{
					SetFailure(OutFailure, ProviderFailure(TEXT("rigging_proposed_asset_missing"), TEXT("The prepared asset is unavailable for publication."),
                        PackagePath, TEXT("proposed_asset_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
					return false;
				}
				Actual = PublishAsset(Proposed.Get(), PackagePath, OutFailure.Diagnostic);
				if (!Actual)
				{
					if (!OutFailure.Diagnostic.IsEmpty()) return false;
					SetFailure(OutFailure, ProviderFailure(TEXT("rigging_asset_publish_failed"), TEXT("The native asset publication operation returned no asset."),
                        PackagePath, TEXT("published_asset_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
					return false;
				}
				FAssetRegistryModule::AssetCreated(Actual);
				Actual->MarkPackageDirty();
				return true;
			}

			virtual bool Compile(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				if (Operation == EOperation::Remove) return true;
				UE::UAI::Contract::FDiagnostic Error;
				if (bCompile && !CompileAsset(Profile.Kind, *(Actual ? Actual : Existing), Error))
				{
					SetFailure(OutFailure, Error);
					return false;
				}
				const auto* Blueprint = Cast<UBlueprint>(Actual ? Actual : Existing);
				return !Blueprint || UeAgentBlueprintAssetLifecycle::ValidateBlueprintPersistence(*Blueprint, OutFailure.Diagnostic);
			}

			virtual bool Save(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				if (!bSave || Operation == EOperation::Remove) return true;
				UE::UAI::Contract::FDiagnostic Error;
				if (!SaveAsset(Profile.Kind, *(Actual ? Actual : Existing), Error))
				{
					SetFailure(OutFailure, Error);
					return false;
				}
				for (const auto& Entry : Dependents)
					if (!UeAgentBlueprintAuthoringOps::SaveBlueprintPackage(Entry.Target.Get(), Error))
					{ if (Error.TargetPath.IsEmpty()) Error.TargetPath = Entry.PackagePath; SetFailure(OutFailure, Error); return false; }
				return true;
			}

			virtual bool BuildDurablePackagePlans(
				TArray<FPackageDurablePlan>& OutPlans,
				FOperationFailure& OutFailure) const override
			{
				if (!BuildPackageDurablePlan(
					PackagePath,
					PackagePath,
					Operation == EOperation::Remove ? Existing
						: (Actual ? Actual : Existing),
					Operation == EOperation::Remove ? EPackageDurableIntent::Delete
						: EPackageDurableIntent::Save,
					OutPlans,
					OutFailure, true,
					Operation == EOperation::Create || Operation == EOperation::Clone
						? EPackageBeforePresence::Absent : EPackageBeforePresence::Present,
					Proposed.IsValid() ? Proposed->GetClass() : nullptr)) return false;
				for (const auto& Entry : Dependents)
					if (!BuildPackageDurablePlan(Entry.PackagePath, Entry.PackagePath, Entry.Target.Get(),
						EPackageDurableIntent::Save, OutPlans, OutFailure)) return false;
				return true;
			}

			virtual bool BuildDomainExpectedReadRequests(
				TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
				FOperationFailure& OutFailure) const override
			{
				return BuildReadRequests(Operation == EOperation::Remove ? nullptr : Proposed.Get(),
					Operation == EOperation::Remove, OutRequests, OutFailure);
			}

			virtual bool BuildDomainActualReadRequests(
				TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
				FOperationFailure& OutFailure) const override
			{
				return BuildReadRequests(Operation == EOperation::Remove ? nullptr : (Actual ? Actual : Existing),
					Operation == EOperation::Remove, OutRequests, OutFailure);
			}

			virtual TSharedPtr<FJsonObject> BuildResultMetadata() const override
			{
				return StatsToJson(Stats);
			}

			virtual bool SupportsPersistentReadback() const override
			{
				return Operation == EOperation::Update && (bSave || Profile.Kind == EAssetKind::ControlRig);
			}

			virtual bool RequiresCheckpointRollback() const override
			{
				// Ordinary authoring documents deliberately omit native member GUIDs
				// and owned-object identities. They are not a rollback representation.
				return Operation == EOperation::Update && Profile.Kind == EAssetKind::ControlRig;
			}

			virtual bool ReleaseForPersistentReadback(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				Proposed.Reset();
				for (auto& Entry : Dependents) Entry.Target.Reset();
				Existing = nullptr;
				Actual = nullptr;
				return true;
			}

			virtual bool RebindAfterPersistentReadback(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				Existing = LoadAsset(Profile.Kind, PackagePath);
				Actual = Existing;
				if (!Existing)
				{
					SetFailure(OutFailure, ProviderFailure(TEXT("rigging_persistent_reload_failed"), TEXT("The saved Rigging asset could not be reloaded."),
                        PackagePath, TEXT("asset_loaded"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
					return false;
				}
				for (auto& Entry : Dependents)
				{
					Entry.Target.Reset(UeAgentBlueprintAssetLifecycle::LoadBlueprint(Entry.PackagePath));
					if (!Entry.Target.IsValid())
					{ SetFailure(OutFailure, ProviderFailure(TEXT("rigging_dependent_reload_failed"), TEXT("A dependent Blueprint could not be reloaded."),
                        Entry.PackagePath, TEXT("asset_loaded"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true))); return false; }
				}
				return true;
			}

			virtual bool Commit(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				if (Operation == EOperation::Remove && RemovalStage.IsStaged())
				{
					UE::UAI::Contract::FDiagnostic Error;
					if (!DeleteAsset(RemovalStage.GetAsset(), PackagePath, Error))
					{
						SetFailure(OutFailure, Error);
						return false;
					}
					RemovalStage.Reset();
					Existing = nullptr;
				}
				return true;
			}

			virtual bool Rollback(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				UE::UAI::Contract::FDiagnostic Error;
				if (Operation == EOperation::Update && Existing && RollbackDocument.IsValid())
				{
					FMutationStats RollbackStats;
					if (!ApplyDocument(Profile.Kind, *Existing, RollbackDocument, RollbackStats, Error))
					{
						SetFailure(OutFailure, Error);
						return false;
					}
					Actual = Existing;
					return true;
				}
				if ((Operation == EOperation::Create || Operation == EOperation::Clone) && Actual)
				{
					if (!DeleteAsset(Actual, PackagePath, Error))
					{
						TArray<UObject*> Objects = { Actual };
						if (ObjectTools::DeleteObjectsUnchecked(Objects) <= 0)
						{
							SetFailure(OutFailure, Error);
							return false;
						}
					}
					Actual = nullptr;
					return true;
				}
				if (Operation == EOperation::Remove && RemovalStage.IsCaptured())
				{
					if (!RemovalStage.Rollback(Error))
					{
						SetFailure(OutFailure, Error);
						return false;
					}
					Actual = Existing;
				}
				return true;
			}

		private:
			bool ApplyUpdateDocument(FOperationFailure& OutFailure)
			{
				UE::UAI::Contract::FDiagnostic Error;
				FMutationStats AppliedStats;
				if (Document && !ApplyDocument(Profile.Kind, *Existing, Document, AppliedStats, Error, CapturedDocument))
				{ SetFailure(OutFailure, Error); return false; }
				Stats = AppliedStats;
				return true;
			}

			virtual void CollectRollbackObjects(TArray<UObject*>& OutObjects) const override
			{
				for (const auto& Entry : Dependents) OutObjects.AddUnique(Entry.Target.Get());
			}

			bool BuildReadRequests(
				UObject* Asset,
				const bool bExpectedAbsent,
				TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
				FOperationFailure& OutFailure) const
			{
				UE::UAI::Contract::FDiagnostic Error;
				if (!BuildAssetSnapshotReadRequests(
					Profile, Asset, PackagePath, bExpectedAbsent, OutRequests, Error))
				{
					SetFailure(OutFailure, Error);
					return false;
				}
				for (const auto& Entry : Dependents)
					if (!UeAgentBlueprintAuthoring::BuildAssetExportSnapshotReadRequests(*Entry.Target.Get(), OutRequests, Error))
					{ SetFailure(OutFailure, Error); return false; }
				return true;
			}

			FProfile Profile;
			struct FDependentBlueprint { FString PackagePath; TStrongObjectPtr<UBlueprint> Target; };
			TArray<FDependentBlueprint> Dependents;
			FString PackagePath;
			EOperation Operation;
			UObject* Existing = nullptr;
			TStrongObjectPtr<UObject> Proposed;
			TSharedPtr<FJsonObject> RollbackDocument;
			FAssetRemovalStage RemovalStage;
			UObject* Actual = nullptr;
			TSharedPtr<const FJsonObject> Document;
			mutable FMutationStats Stats;
			bool bCompile = true;
			bool bSave = false;
			bool bDocumentNoOp = false;
			TSharedPtr<const FJsonObject> CapturedDocument;
		};

		class FAssetLifecycle final : public IReflectionOperationLifecycle
		{
		public:
			explicit FAssetLifecycle(TSharedPtr<const FJsonObject> InCapturedDocument)
				: CapturedDocument(MoveTemp(InCapturedDocument)) {}
			virtual bool PrepareLifecycle(
				const FPrepareRequest& Request,
				TUniquePtr<IReflectionOperationLifecycleState>& OutState,
				FOperationFailure& OutFailure) const override
			{
				OutState.Reset();
				OutFailure = FOperationFailure();
                const auto InputFailure = [&](const TCHAR* Code, const FString& Reason, const FString& Pointer,
                    const TSharedPtr<FJsonValue>& ActualValue, const TSharedPtr<FJsonValue>& ExpectedValue, const FString& TargetPath) {
                    auto Error = ProviderFailure(Code, Reason, TargetPath, Pointer, ActualValue, ExpectedValue);
                    Error.SourceKind = TEXT("authored_value");
                    Error.Source.JsonPath = Pointer;
                    Error.Source.InternalJsonPath = Pointer;
                    if (Request.Unit) Error.Source = Request.Unit->ResolveDiagnosticSource(Pointer);
                    return Error;
                };
				if (!Request.IsValid() || !Request.Input->Target.IsValid() || !Request.Contract)
				{
					auto ActualState = MakeShared<FJsonObject>();
                    ActualState->SetBoolField(TEXT("request_valid"), Request.IsValid());
                    ActualState->SetBoolField(TEXT("target_present"), Request.Input && Request.Input->Target.IsValid());
                    ActualState->SetBoolField(TEXT("contract_present"), Request.Contract != nullptr);
                    SetFailure(OutFailure, ProviderFailure(TEXT("rigging_apply_request_invalid"), TEXT("The internal apply request requires a valid request, target and contract."),
                        FString(), TEXT("request"), MakeShared<FJsonValueObject>(ActualState), MakeShared<FJsonValueString>(TEXT("valid request with target and contract"))));
					return false;
				}
				const FProfile* Profile = FindProfileByTargetKind(Request.Contract->TargetKind);
				if (!Profile)
				{
					SetFailure(OutFailure, ProviderFailure(TEXT("rigging_apply_profile_missing"), TEXT("No Rigging profile is registered for the apply contract target kind."),
                        FString(), TEXT("target_kind"), MakeShared<FJsonValueString>(Request.Contract->TargetKind),
                        MakeShared<FJsonValueString>(TEXT("registered Rigging target kind"))));
					return false;
				}
				FString PackagePath;
				Request.Input->Target->TryGetStringField(TEXT("asset_path"), PackagePath);
				PackagePath = NormalizePackagePath(PackagePath);
				if (!FPackageName::IsValidLongPackageName(PackagePath)
					|| FPackageName::GetLongPackageAssetName(PackagePath).IsEmpty()
					|| (!PackagePath.Equals(TEXT("/Game")) && !PackagePath.StartsWith(TEXT("/Game/"))))
				{
					SetFailure(OutFailure, InputFailure(TEXT("invalid_asset_path"), TEXT("asset_path must name a valid asset package under /Game."),
                        TEXT("/target/asset_path"), Request.Input->Target->TryGetField(TEXT("asset_path")),
                        MakeShared<FJsonValueString>(TEXT("valid /Game/... asset package path")), FString()));
					return false;
				}
				FString AssetKind;
				if (Request.Input->Target->TryGetStringField(TEXT("asset_kind"), AssetKind)
					&& !AssetKind.Equals(Profile->TypeId, ESearchCase::IgnoreCase))
				{
					SetFailure(OutFailure, InputFailure(TEXT("rigging_asset_kind_conflict"), TEXT("asset_kind does not match the selected Rigging profile."),
                        TEXT("/target/asset_kind"), MakeShared<FJsonValueString>(AssetKind), MakeShared<FJsonValueString>(Profile->TypeId), PackagePath));
					return false;
				}

				UObject* Existing = LoadAsset(Profile->Kind, PackagePath);
				EOperation Operation = Request.Input->RequestedOperation;
				if (Operation == EOperation::Upsert) Operation = Existing ? EOperation::Update : EOperation::Create;
				if ((Operation == EOperation::Create || Operation == EOperation::Clone) && Existing)
				{
					SetFailure(OutFailure, InputFailure(TEXT("asset_already_exists"), TEXT("Create and clone require a destination asset that does not already exist."),
                        TEXT("/target/asset_path"), MakeShared<FJsonValueString>(PackagePath), MakeShared<FJsonValueString>(TEXT("unused asset path")), PackagePath));
					return false;
				}
				if ((Operation == EOperation::Update || Operation == EOperation::Remove) && !Existing)
				{
					SetFailure(OutFailure, InputFailure(TEXT("asset_not_found"), TEXT("Update and remove require an existing asset of the selected Rigging kind."),
                        TEXT("/target/asset_path"), MakeShared<FJsonValueString>(PackagePath), MakeShared<FJsonValueString>(Profile->TypeId + TEXT(" asset")), PackagePath));
					return false;
				}

				TSharedPtr<FJsonObject> Document;
				UE::UAI::Contract::FDiagnostic Error;
				if (!ExtractDocument(Profile->Kind, Request.Input->ProviderExtension, Document, Error))
				{
                    if (Request.Unit) Error.Source = Request.Unit->ResolveDiagnosticSource(Error.Source.InternalJsonPath);
                    Error.TargetPath = ToObjectPath(PackagePath);
					SetFailure(OutFailure, Error);
					return false;
				}
				if (Operation != EOperation::Update && !Document.IsValid())
				{
					Document = MakeShared<FJsonObject>();
				}
				UObject* Source = nullptr;
				if (Operation == EOperation::Clone)
				{
					FString SourcePath;
					Request.Input->Target->TryGetStringField(TEXT("source_asset_path"), SourcePath);
					Source = LoadAsset(Profile->Kind, SourcePath);
					if (!Source)
					{
						SetFailure(OutFailure, InputFailure(TEXT("source_asset_not_found"), TEXT("The clone source could not be loaded as the selected Rigging asset kind."),
                            TEXT("/target/source_asset_path"), MakeShared<FJsonValueString>(SourcePath),
                            MakeShared<FJsonValueString>(Profile->TypeId + TEXT(" asset")), PackagePath));
						return false;
					}
				}

				UObject* Proposed = nullptr;
				FMutationStats Stats;
				if (Operation != EOperation::Remove)
				{
					UPackage* Scratch = CreatePackage(*FString::Printf(
						TEXT("/Temp/UAI/Rigging/Proposed_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
					Proposed = Operation == EOperation::Create
						? CreateFactorySeed(Profile->Kind, Scratch, NAME_None, RF_Transactional, Error)
						: DuplicateAsset(Operation == EOperation::Clone ? Source : Existing,
							Scratch, NAME_None, RF_Transactional, Error);
					if (!Proposed)
					{
						if (Error.IsEmpty()) Error = ProviderFailure(TEXT("rigging_proposed_asset_create_failed"),
                            TEXT("The native asset duplication operation returned no prepared asset."), PackagePath, TEXT("proposed_asset_available"),
                            MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
                        if (Error.TargetPath.IsEmpty()) Error.TargetPath = ToObjectPath(PackagePath);
                        SetFailure(OutFailure, Error);
						return false;
					}
					// Duplication runs PostLoad; IK assets restore RF_Standalone there.
					// This object belongs only to the proposed-state lifetime, not the asset registry.
					Proposed->ClearFlags(RF_Public | RF_Standalone);
					Proposed->SetFlags(RF_Transient);
					if (auto* Blueprint = Cast<UBlueprint>(Proposed))
						UeAgentBlueprintAssetLifecycle::SetAuthoringContext(*Blueprint,
							PackagePath, Cast<UBlueprint>(Existing ? Existing : Source));
					// An Update without document fields preserves the duplicated native document.
					// Create/Clone still exercise their existing document validation and dependencies.
					if ((Operation != EOperation::Update || Document.IsValid())
						&& !ApplyDocument(Profile->Kind, *Proposed, Document, Stats, Error, CapturedDocument, PackagePath))
					{
						SetFailure(OutFailure, Error);
						return false;
					}
				}

				UE::UAI::Contract::FDiagnostic ComparisonError;
				const bool bDocumentNoOp = Operation == EOperation::Update
					&& Existing && Proposed && DocumentsEqual(*Existing, *Proposed, *Profile, ComparisonError, PackagePath);
				if (!ComparisonError.IsEmpty())
				{
					if (ComparisonError.TargetPath.IsEmpty()) ComparisonError.TargetPath = PackagePath;
					UeAgentAuthoringDiagnostics::ReportConsistencyWarning(ComparisonError);
				}
				TArray<UBlueprint*> Dependents;
				if (auto* Rig = Cast<UControlRigBlueprint>(Existing); Rig && Document)
				{
					const TSharedPtr<FJsonObject>* Variables = nullptr;
					if (Document->TryGetObjectField(TEXT("variables"), Variables)
						&& !UeAgentControlRigOps::CollectVariableTypeChangeDependents(*Rig, **Variables, Dependents, Error))
					{
						UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(Error, TEXT("/variables"));
						UeAgentAuthoringDiagnostics::LocateFailure(Error, *Document, TEXT("variable_type_decode"), PackagePath, TEXT("variables"));
						SetFailure(OutFailure, Error);
						return false;
					}
				}
				OutState = MakeUnique<FAssetLifecycleState>(
					*Profile, PackagePath, Operation, Existing, Proposed, Document, Stats,
					ReadBool(Request.Input->ProviderExtension, TEXT("compile"), true),
					ReadBool(Request.Input->ProviderExtension, TEXT("save"), false),
					bDocumentNoOp, CapturedDocument, Dependents);
				return true;
			}
		private:
			TSharedPtr<const FJsonObject> CapturedDocument;
		};

		FInputFieldDescriptor Field(
			const TCHAR* Name,
			const TCHAR* Role,
			const TCHAR* TypeId,
			const TArray<EOperation>& RequiredFor = {})
		{
			FInputFieldDescriptor Result;
			Result.Name = Name;
			Result.Role = Role;
			Result.TypeId = TypeId;
			Result.RequiredFor = RequiredFor;
			return Result;
		}

		FOperationDescriptor OperationDescriptor(const EOperation Operation)
		{
			FOperationDescriptor Result;
			Result.Operation = Operation;
			Result.IdentityOneOf = { { TEXT("asset_path") } };
			Result.AcceptedSections = { TEXT("target"), TEXT("provider_extension") };
			if (Operation != EOperation::Remove) Result.AcceptedSections.Add(TEXT("properties"));
			if (Operation == EOperation::Create)
			{
				Result.OmittedFieldPolicy = TEXT("ue_native_default");
				Result.NativeDefaultPolicy = TEXT("required");
			}
			else if (Operation == EOperation::Update)
			{
				Result.OmittedFieldPolicy = TEXT("preserve_current");
				Result.NativeDefaultPolicy = TEXT("forbidden");
			}
			else if (Operation == EOperation::Clone || Operation == EOperation::Upsert)
			{
				Result.OmittedFieldPolicy = TEXT("ue_native_default");
				Result.NativeDefaultPolicy = TEXT("allowed");
			}
			else
			{
				Result.OmittedFieldPolicy = TEXT("not_applicable");
				Result.NativeDefaultPolicy = TEXT("not_applicable");
			}
			Result.bDestructive = Operation == EOperation::Remove;
			return Result;
		}

		FContract MakeInputContract(const FProfile& Profile)
		{
			FContract Result;
			Result.Schema = TEXT("uai.apply_input_contract.v1");
			Result.ContractId = Profile.InputContractId;
			Result.ContractVersion = TEXT("1.0.0");
			Result.ProfileId = Profile.ProfileId;
			Result.TargetKind = Profile.TargetKind;
			Result.SemanticDescriptorRefs = { Profile.DocumentSemanticId, Profile.ExportTargetSemanticId };
			Result.ReflectionBaseline.bEnabled = true;
			Result.ReflectionBaseline.RootKinds = { TEXT("uobject"), TEXT("ustruct") };
			Result.ReflectionBaseline.RecursiveNodes = {
				TEXT("scalar"), TEXT("struct"), TEXT("owned_object"), TEXT("reference"),
				TEXT("array"), TEXT("set"), TEXT("map"), TEXT("fixed_array"), TEXT("optional")
			};
			Result.ReflectionBaseline.UnknownFieldPolicy = TEXT("reject");
			Result.ReflectionBaseline.ExternalReferencePolicy = TEXT("replace_reference_only");
			Result.ReflectionBaseline.OwnedObjectPolicy = TEXT("create_or_locate_with_real_outer_then_recurse");
			Result.PropertyPayload.Mode = TEXT("reflection");
			Result.PropertyPayload.PropertyPathField = TEXT("property_path");
			Result.PropertyPayload.ValueField = TEXT("value");
			Result.PropertyPayload.OptionalAssertionFields = {
				TEXT("kind"), TEXT("struct_path"), TEXT("enum_path"), TEXT("element_type"),
				TEXT("key_type"), TEXT("mapped_type"), TEXT("inner_type"), TEXT("fixed_length"), TEXT("allowed_class")
			};
			Result.PropertyPayload.OmittedPatchFieldPolicy = TEXT("preserve_current");
			const TArray<EOperation> All = {
				EOperation::Create, EOperation::Update, EOperation::Upsert, EOperation::Remove, EOperation::Clone
			};
			Result.InputFields = {
				Field(TEXT("asset_path"), TEXT("identity"), TEXT("ue_asset_path"), All),
				Field(TEXT("asset_kind"), TEXT("type_assertion"), *Profile.TypeId),
				Field(TEXT("source_asset_path"), TEXT("selector"), TEXT("ue_asset_path"), { EOperation::Clone }),
				Field(TEXT("operation"), TEXT("operation_control"), TEXT("apply_operation"), All),
				Field(TEXT("properties"), TEXT("intent"), TEXT("property_descriptor_list")),
				Field(TEXT("compile"), TEXT("provider_extension"), TEXT("bool")),
				Field(TEXT("save"), TEXT("provider_extension"), TEXT("bool")),
			};
			for (const FString& Name : GetDocumentFields(Profile.Kind))
				Result.InputFields.Add(Field(*Name, TEXT("provider_extension"), TEXT("object_or_typed_value")));
			for (const EOperation Item : All) Result.Operations.Add(OperationDescriptor(Item));
			Result.ProviderId = ApplyProviderId;
			Result.UnknownInputFieldPolicy = TEXT("reject");
			Result.Verification.StateSource = TEXT("shared_state_snapshot");
			Result.Verification.CompareSource = TEXT("shared_semantic_compare");
			Result.Verification.bRequired = true;
			return Result;
		}

		UeAgentApplyWorkspaceContract::FContract MakeWorkspaceContract(const FProfile& Profile)
		{
			using namespace UeAgentApplyWorkspaceContract;
			UeAgentApplyWorkspaceContract::FContract Result;
			Result.Schema = TEXT("uai.apply_workspace_contract.v1");
			Result.ContractId = Profile.WorkspaceContractId;
			Result.ContractVersion = TEXT("1.0.0");
			Result.ProfileId = Profile.ProfileId;
			Result.Sources = {
				{ ESourceKind::Directory, TEXT("physical"), TEXT("index") },
				{ ESourceKind::SingleFile, TEXT("virtual"), TEXT("object") },
				{ ESourceKind::Inline, TEXT("virtual"), TEXT("object") },
			};
			Result.Entry.DirectoryEntry = TEXT("index.hjson");
			Result.Entry.VirtualUnitId = TEXT("asset");
			FUnitDescriptor Unit;
			Unit.UnitId = TEXT("asset");
			Unit.PathPattern = TEXT("index.hjson");
			Unit.Role = TEXT("object");
			Unit.Cardinality = TEXT("one");
			Unit.bRequired = true;
			Unit.RouteId = Profile.ProfileId;
			Unit.ApplyContractRef = Profile.InputContractId;
			Unit.MergePolicy = TEXT("replace");
			Unit.AbsentSemantics = TEXT("error");
			Unit.EmptySemantics = TEXT("error");
			Result.Units = { Unit };
			Result.ReferencePolicy.Base = TEXT("workspace_root");
			Result.ReferencePolicy.RootContainment = TEXT("required");
			Result.ReferencePolicy.ExternalReferences = TEXT("reject");
			Result.ReferencePolicy.CaseCollision = TEXT("reject");
			Result.DependencyPolicy.EdgeSource = TEXT("contract_and_references");
			Result.DependencyPolicy.CyclePolicy = TEXT("reject");
			Result.DependencyPolicy.UnresolvedPolicy = TEXT("reject");
			Result.DependencyPolicy.Order = TEXT("stable_topological");
			Result.Preflight.Scope = TEXT("whole_workspace");
			Result.Preflight.MutationBeforeSuccess = TEXT("forbid");
			Result.Preflight.bDiagnosticsIncludeSource = true;
			Result.Transaction.Mode = TEXT("atomic");
			Result.Transaction.RollbackOn = {
				TEXT("mutation_failed"), TEXT("compile_failed"), TEXT("save_failed"), TEXT("verification_failed")
			};
			Result.Execution.Compile = TEXT("provider_hook");
			Result.Execution.Save = TEXT("caller_controlled");
			Result.Execution.Verification = TEXT("shared_snapshot_required");
			Result.UnknownFilePolicy = TEXT("reject");
			Result.DuplicateTargetPolicy = TEXT("reject");
			return Result;
		}

		TSharedRef<FJsonObject> MakeApplyDocument(const FAssetApplyRequest& Request)
		{
			TSharedRef<FJsonObject> Target = MakeShared<FJsonObject>();
			Target->SetStringField(TEXT("asset_path"), Request.AssetPath);
			Target->SetStringField(TEXT("asset_kind"), AssetKindToString(Request.Kind));
			if (!Request.SourceAssetPath.IsEmpty())
				Target->SetStringField(TEXT("source_asset_path"), Request.SourceAssetPath);
			TSharedRef<FJsonObject> Extension = Request.Document.IsValid()
				? MakeShared<FJsonObject>(Request.Document->Values) : MakeShared<FJsonObject>();
			Extension->SetBoolField(TEXT("compile"), Request.bCompile);
			Extension->SetBoolField(TEXT("save"), Request.bSave);
			TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
			Document->SetStringField(TEXT("operation"), OperationToString(Request.Operation));
			Document->SetObjectField(TEXT("target"), Target);
			Document->SetObjectField(TEXT("provider_extension"), Extension);
			if (!Request.Properties.IsEmpty()) Document->SetArrayField(TEXT("properties"), Request.Properties);
			return Document;
		}

		bool LoadWorkspace(
			const FProfile& Profile,
			const UeAgentAuthoringWorkspaceLoader::FSourceRequest& Source,
			UeAgentApplyWorkspaceContract::FContract& OutContract,
			UeAgentAuthoringWorkspaceLoader::FLoadResult& OutLoad,
			UE::UAI::Contract::FDiagnostic& OutDiagnostic)
		{
			if (!UeAgentApplyWorkspaceContract::FRegistry::Get().FindContract(
				Profile.WorkspaceContractId, OutContract))
			{
				OutDiagnostic = ProviderFailure(TEXT("rigging_workspace_contract_missing"),
                    TEXT("The workspace contract for Rigging profile '") + Profile.TypeId + TEXT("' is not registered."),
                    FString(), TEXT("workspace_contract_id"), MakeShared<FJsonValueString>(Profile.WorkspaceContractId),
                    MakeShared<FJsonValueString>(TEXT("registered workspace contract")));
				return false;
			}
			if (!UeAgentAuthoringWorkspaceLoader::LoadWorkspace(OutContract, Source, OutLoad))
			{
				OutDiagnostic = OutLoad.Diagnostic;
				return false;
			}
			return true;
		}

		bool BuildDependencyGraph(
			const UeAgentApplyWorkspaceContract::FContract& Contract,
			const UeAgentAuthoringWorkspaceRouter::FWorkspace& Workspace,
			UeAgentAuthoringWorkspaceDependencyGraph::FDependencyGraph& OutGraph,
			UE::UAI::Contract::FDiagnostic& OutDiagnostic)
		{
			UeAgentAuthoringWorkspaceDependencyGraph::FGraphFailure Failure;
			if (UeAgentAuthoringWorkspaceDependencyGraph::BuildDependencyGraph(
				Contract, Workspace, UeAgentAuthoringWorkspaceDependencyGraph::FDependencyInputs(),
				OutGraph, Failure)) return true;
			OutDiagnostic = Failure.Diagnostic;
			return false;
		}

		bool InspectWorkspaceSource(
			const FProfile& Profile,
			const UeAgentAuthoringWorkspaceLoader::FSourceRequest& Source,
			const FString& ExpectedAssetPath,
			FWorkspaceInspectResult& OutResult)
		{
			OutResult = FWorkspaceInspectResult();
			OutResult.Status = TEXT("failed");
			UeAgentApplyWorkspaceContract::FContract Contract;
			UeAgentAuthoringWorkspaceLoader::FLoadResult Load;
			if (!LoadWorkspace(Profile, Source, Contract, Load, OutResult.Diagnostic)) return false;
			UeAgentAuthoringAssetWorkspaceInput::FTargetRequest TargetRequest;
			TargetRequest.ExpectedAssetPath = ExpectedAssetPath;
			TargetRequest.Errors.EntryInvalid = TEXT("rigging_workspace_entry_invalid");
			TargetRequest.Errors.AssetPathConflict =
				TEXT("rigging_workspace_asset_path_conflict");
			UeAgentAuthoringAssetWorkspaceInput::FTargetResult Target;
			if (!UeAgentAuthoringAssetWorkspaceInput::ResolveTarget(
				Load.Workspace, TargetRequest, Target))
			{
				OutResult.Diagnostic = Target.Diagnostic;
				return false;
			}
			const EOperation Operation = Target.Input.RequestedOperation;
			OutResult.AssetPath = Target.AssetPath;
			OutResult.RequestedOperation = OperationToString(Operation);
			OutResult.bAssetExists = LoadAsset(Profile.Kind, OutResult.AssetPath) != nullptr;
			OutResult.bWouldCreate = Operation == EOperation::Create
				|| (Operation == EOperation::Upsert && !OutResult.bAssetExists);
			UeAgentAuthoringWorkspaceDependencyGraph::FDependencyGraph Graph;
			if (!BuildDependencyGraph(Contract, Load.Workspace, Graph,
				OutResult.Diagnostic)) return false;
			FWorkspaceOperationPreflightProvider Provider(
				UeAgentApplyInputContract::FRegistry::Get(), FProviderRegistry::Get());
			UeAgentAuthoringWorkspaceTransaction::FPreflightResult Preflight;
			if (!UeAgentAuthoringWorkspaceTransaction::PreflightWorkspace(
				Contract, Load.Workspace, Graph, Provider, Preflight))
			{
				const FApplyFailureProjection Failure = ProjectApplyFailure(Preflight);
				OutResult.UnitsChecked = Preflight.UnitsChecked;
				OutResult.Diagnostic = Failure.Diagnostic;
				return false;
			}
			OutResult.UnitsChecked = Preflight.UnitsChecked;
			if (Preflight.Units.Num() == 1 && Preflight.Units[0].Mutation.IsValid())
			{
				OutResult.bNoOp = Preflight.Units[0].Mutation->IsNoOp();
				OutResult.PlannedStats = StatsFromJson(Preflight.Units[0].Mutation->BuildResultMetadata());
			}
			OutResult.Status = TEXT("ready");
			return true;
		}

		bool ExecuteWorkspaceSource(
			const FProfile& Profile,
			const UeAgentAuthoringWorkspaceLoader::FSourceRequest& Source,
			const FString& ExpectedAssetPath,
			FAssetApplyResult& OutResult,
			const FString& RequestId,
			TFunction<bool(FOperationFailure&)> FinalReadback = {},
			TSharedPtr<const FJsonObject> CapturedDocument = nullptr)
		{
			OutResult = FAssetApplyResult();
			OutResult.Status = TEXT("failed");
			UeAgentApplyWorkspaceContract::FContract Contract;
			UeAgentAuthoringWorkspaceLoader::FLoadResult Load;
			if (!LoadWorkspace(Profile, Source, Contract, Load, OutResult.Diagnostic))
			{
				OutResult.Phase = TEXT("load");
				return false;
			}
			UeAgentAuthoringAssetWorkspaceInput::FTargetRequest TargetRequest;
			TargetRequest.ExpectedAssetPath = ExpectedAssetPath;
			TargetRequest.Errors.EntryInvalid = TEXT("rigging_workspace_entry_invalid");
			TargetRequest.Errors.AssetPathConflict =
				TEXT("rigging_workspace_asset_path_conflict");
			UeAgentAuthoringAssetWorkspaceInput::FTargetResult Target;
			if (!UeAgentAuthoringAssetWorkspaceInput::ResolveTarget(
				Load.Workspace, TargetRequest, Target))
			{
				OutResult.Phase = Target.Phase;
				OutResult.Diagnostic = Target.Diagnostic;
				return false;
			}
			const FOperationInput& OperationInput = Target.Input;
			const EOperation RequestedOperation = OperationInput.RequestedOperation;
			OutResult.AssetPath = Target.AssetPath;
			const bool bExisted = LoadAsset(Profile.Kind, OutResult.AssetPath) != nullptr;
			bool bSave = false;
			if (OperationInput.ProviderExtension.IsValid())
				OperationInput.ProviderExtension->TryGetBoolField(TEXT("save"), bSave);
			FBackendOptions BackendOptions;
			BackendOptions.FinalReadback = MoveTemp(FinalReadback);
			BackendOptions.bUseEditorTransaction = RequestedOperation != EOperation::Remove;
			BackendOptions.TransactionDescription = NSLOCTEXT(
				"UeAgentInterface", "RiggingAssetWorkspaceApply", "Apply rigging asset workspace");
			FApplyWorkspaceResult ApplyResult;
			UeAgentAuthoringAssetWorkspaceApply::FFailure ApplyFailure;
			FProviderRegistry LocalProviders;
			const FProviderRegistry* Providers = &FProviderRegistry::Get();
			if (CapturedDocument)
			{
				UeAgentAuthoringOperationApply::ERegistrationResult Registered;
				UE::UAI::Contract::FDiagnostic RegistrationError;
				if (!LocalProviders.RegisterProvider(MakeApplyProvider(CapturedDocument), Registered, RegistrationError))
				{ OutResult.Phase = TEXT("preflight"); OutResult.Diagnostic = RegistrationError; if (OutResult.Diagnostic.Stage.IsEmpty()) OutResult.Diagnostic.Stage = OutResult.Phase; return false; }
				Providers = &LocalProviders;
			}
			const bool bApplied = UeAgentAuthoringAssetWorkspaceApply::Execute(
				Contract, Load.Workspace, *Providers, RequestId,
				bSave, BackendOptions, ApplyResult, ApplyFailure);
			OutResult.UnitsChecked = ApplyResult.Preflight.UnitsChecked;
			OutResult.MutationsApplied = ApplyResult.Execution.MutationsApplied;
			OutResult.bRollbackAttempted = ApplyResult.Execution.bRollbackAttempted;
			OutResult.bRollbackSucceeded = ApplyResult.Execution.bRollbackSucceeded;
			if (!bApplied)
			{
				OutResult.Phase = ApplyFailure.Phase;
				OutResult.Diagnostic = ApplyFailure.Diagnostic;
				return false;
			}
			const EOperation Resolved = RequestedOperation == EOperation::Upsert
				? (bExisted ? EOperation::Update : EOperation::Create) : RequestedOperation;
			OutResult.Status = TEXT("applied");
			OutResult.Phase = TEXT("complete");
			OutResult.ResolvedOperation = OperationToString(Resolved);
			OutResult.Asset = Resolved == EOperation::Remove ? nullptr : LoadAsset(Profile.Kind, OutResult.AssetPath);
			OutResult.ObjectPath = OutResult.Asset ? OutResult.Asset->GetPathName() : ToObjectPath(OutResult.AssetPath);
			if (const TSharedPtr<FJsonObject>* Metadata = ApplyResult.UnitResultMetadata.Find(TEXT("asset"));
				Metadata && Metadata->IsValid()) OutResult.Stats = StatsFromJson(*Metadata);
			return true;
		}
	}

	bool RegisterApplyContracts(UE::UAI::Contract::FDiagnostic& OutError)
	{
		for (const FProfile& Profile : GetProfiles())
		{
			UeAgentApplyInputContract::ERegistrationResult InputResult;
			if (!UeAgentApplyInputContract::FRegistry::Get().RegisterContract(
				MakeInputContract(Profile), InputResult, OutError)) return false;
			UeAgentApplyWorkspaceContract::ERegistrationResult WorkspaceResult;
			if (!UeAgentApplyWorkspaceContract::FRegistry::Get().RegisterContract(
				MakeWorkspaceContract(Profile), WorkspaceResult, OutError)) return false;
		}
		return true;
	}

	TSharedRef<IApplyProvider> MakeApplyProvider(TSharedPtr<const FJsonObject> CapturedDocument)
	{
		const TSharedRef<IReflectionOperationLifecycle> Lifecycle = MakeShared<FAssetLifecycle>(MoveTemp(CapturedDocument));
		UeAgentAuthoringObjectApply::FOptions Options;
		Options.bAllowDomainAuthorizedOwners = true;
		return MakeShared<FReflectionApplyProvider>(ApplyProviderId, Lifecycle, Options);
	}

	bool ApplyAsset(const FAssetApplyRequest& Request, FAssetApplyResult& OutResult, const FString& RequestId)
	{
		const FProfile* Profile = FindProfile(Request.Kind);
		if (!Profile)
		{
			OutResult = FAssetApplyResult();
			OutResult.Diagnostic = ProviderFailure(TEXT("rigging_apply_profile_missing"), TEXT("No Rigging apply profile is registered for the requested asset kind."),
                Request.AssetPath, TEXT("asset_kind"), MakeShared<FJsonValueNumber>(static_cast<int32>(Request.Kind)),
                MakeShared<FJsonValueString>(TEXT("ControlRig, IKRig or IKRetargeter")));
			return false;
		}
		return ExecuteWorkspaceSource(
			*Profile,
			UeAgentAuthoringWorkspaceLoader::FSourceRequest::MakeInlineObject(
				MakeApplyDocument(Request), TEXT("inline://rigging-asset-apply")),
			Request.AssetPath,
			OutResult,
			RequestId,
			Request.FinalReadback, Request.CapturedDocument);
	}

	bool ApplyAssetWorkspace(
		const FProfile& Profile,
		const FString& WorkspaceRoot,
		const FString& ExpectedAssetPath,
		FAssetApplyResult& OutResult,
		const FString& RequestId)
	{
		return ExecuteWorkspaceSource(
			Profile,
			UeAgentAuthoringWorkspaceLoader::FSourceRequest::MakeDirectory(WorkspaceRoot),
			ExpectedAssetPath,
			OutResult,
			RequestId);
	}

	bool InspectAssetWorkspace(
		const FProfile& Profile,
		const FString& WorkspaceRoot,
		const FString& ExpectedAssetPath,
		FWorkspaceInspectResult& OutResult)
	{
		return InspectWorkspaceSource(
			Profile,
			UeAgentAuthoringWorkspaceLoader::FSourceRequest::MakeDirectory(WorkspaceRoot),
			ExpectedAssetPath,
			OutResult);
	}
}

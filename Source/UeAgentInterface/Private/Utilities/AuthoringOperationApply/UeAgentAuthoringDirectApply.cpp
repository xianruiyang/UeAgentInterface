// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringOperationApply/UeAgentAuthoringDirectApply.h"

#include "Utilities/AuthoringOperationApply/UeAgentAuthoringObjectApplyProvider.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceDependencyGraph.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/Class.h"
#include "UObject/Object.h"

namespace
{
	using namespace UeAgentApplyInputContract;
	using namespace UeAgentAuthoringOperationApply;

	const FString DirectInputContractId = TEXT("uai.apply_input.core.reflection_patch");
	const FString DirectProviderId = TEXT("core.reflection_patch");

	FContract MakeInputContract()
	{
		FContract Result;
		Result.Schema = TEXT("uai.apply_input_contract.v1");
		Result.ContractId = DirectInputContractId;
		Result.ContractVersion = TEXT("1.0.0");
		Result.ProfileId = TEXT("core.reflection");
		Result.TargetKind = TEXT("uobject_or_ustruct");
		Result.SemanticDescriptorRefs = { TEXT("core.property") };
		Result.ReflectionBaseline.bEnabled = true;
		Result.ReflectionBaseline.RootKinds = { TEXT("uobject"), TEXT("ustruct") };
		Result.ReflectionBaseline.RecursiveNodes = {
			TEXT("scalar"), TEXT("struct"), TEXT("owned_object"), TEXT("reference"), TEXT("array"),
			TEXT("set"), TEXT("map"), TEXT("fixed_array"), TEXT("optional")
		};
		Result.ReflectionBaseline.UnknownFieldPolicy = TEXT("reject");
		Result.ReflectionBaseline.ExternalReferencePolicy = TEXT("replace_reference_only");
		Result.ReflectionBaseline.OwnedObjectPolicy = TEXT("create_or_locate_with_real_outer_then_recurse");
		Result.ReflectionBaseline.RuntimeLimitsSource = TEXT("shared_runtime_limits");
		Result.PropertyPayload.Mode = TEXT("reflection");
		Result.PropertyPayload.PropertyPathField = TEXT("property_path");
		Result.PropertyPayload.ValueField = TEXT("value");
		Result.PropertyPayload.OptionalAssertionFields = {
			TEXT("value_kind"), TEXT("struct_path"), TEXT("enum_path"), TEXT("element_type"),
			TEXT("key_type"), TEXT("mapped_type"), TEXT("inner_type"), TEXT("fixed_length"), TEXT("allowed_class")
		};
		Result.PropertyPayload.OmittedPatchFieldPolicy = TEXT("preserve_current");

		FInputFieldDescriptor TargetPath;
		TargetPath.Name = TEXT("path");
		TargetPath.Role = TEXT("identity");
		TargetPath.TypeId = TEXT("ue_object_or_struct_path");
		TargetPath.RequiredFor = { EOperation::Update };
		FInputFieldDescriptor Operation;
		Operation.Name = TEXT("operation");
		Operation.Role = TEXT("operation_control");
		Operation.TypeId = TEXT("apply_operation");
		Operation.RequiredFor = { EOperation::Update };
		FInputFieldDescriptor Properties;
		Properties.Name = TEXT("properties");
		Properties.Role = TEXT("intent");
		Properties.TypeId = TEXT("property_descriptor_list");
		Result.InputFields = { TargetPath, Operation, Properties };

		FOperationDescriptor Update;
		Update.Operation = EOperation::Update;
		Update.IdentityOneOf = { { TEXT("path") } };
		Update.AcceptedSections = { TEXT("target"), TEXT("properties") };
		Update.OmittedFieldPolicy = TEXT("preserve_current");
		Update.NativeDefaultPolicy = TEXT("forbidden");
		Result.Operations = { Update };
		Result.ProviderId = DirectProviderId;
		Result.UnknownInputFieldPolicy = TEXT("reject");
		Result.Verification.StateSource = TEXT("shared_state_snapshot");
		Result.Verification.CompareSource = TEXT("shared_semantic_compare");
		Result.Verification.bRequired = true;
		return Result;
	}

	UeAgentApplyWorkspaceContract::FContract MakeWorkspaceContract()
	{
		using namespace UeAgentApplyWorkspaceContract;
		UeAgentApplyWorkspaceContract::FContract Result;
		Result.Schema = TEXT("uai.apply_workspace_contract.v1");
		Result.ContractId = TEXT("uai.apply_workspace.core_reflection_patch");
		Result.ContractVersion = TEXT("1.0.0");
		Result.ProfileId = TEXT("core.reflection");
		Result.Sources = {
			{ ESourceKind::Directory, TEXT("physical"), TEXT("index") },
			{ ESourceKind::SingleFile, TEXT("virtual"), TEXT("object") },
			{ ESourceKind::Inline, TEXT("virtual"), TEXT("object") }
		};
		Result.Entry.DirectoryEntry = TEXT("index.hjson");
		Result.Entry.VirtualUnitId = TEXT("primary");
		FUnitDescriptor Unit;
		Unit.UnitId = TEXT("primary");
		Unit.PathPattern = TEXT("index.hjson");
		Unit.Role = TEXT("object");
		Unit.Cardinality = TEXT("one");
		Unit.bRequired = true;
		Unit.RouteId = TEXT("core.reflection_patch");
		Unit.ApplyContractRef = DirectInputContractId;
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
		Result.Execution.Compile = TEXT("none");
		Result.Execution.Save = TEXT("none");
		Result.Execution.Verification = TEXT("shared_snapshot_required");
		Result.UnknownFilePolicy = TEXT("reject");
		Result.DuplicateTargetPolicy = TEXT("reject");
		return Result;
	}

	class FDirectLifecycleState final : public IReflectionOperationLifecycleState
	{
	public:
		FDirectLifecycleState(FString InTargetKey, FReflectionRoot InRoot)
			: TargetKey(MoveTemp(InTargetKey))
			, Root(MoveTemp(InRoot))
		{
		}

		virtual FString GetTargetKey() const override { return TargetKey; }
		virtual EOperation GetResolvedOperation() const override { return EOperation::Update; }
		virtual bool IsNoOp() const override { return true; }
		virtual FReflectionRoot GetProposedRoot() const override { return Root; }
		virtual FReflectionRoot GetActualRoot() const override { return Root; }
		virtual bool CaptureRollback(FOperationFailure& OutFailure) override { OutFailure = FOperationFailure(); return true; }
		virtual bool ApplyLifecycle(FOperationFailure& OutFailure) override { OutFailure = FOperationFailure(); return true; }
		virtual bool Compile(FOperationFailure& OutFailure) override { OutFailure = FOperationFailure(); return true; }
		virtual bool Save(FOperationFailure& OutFailure) override { OutFailure = FOperationFailure(); return true; }
		virtual bool BuildDomainExpectedReadRequests(
			TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
			FOperationFailure& OutFailure) const override { OutFailure = FOperationFailure(); return true; }
		virtual bool BuildDomainActualReadRequests(
			TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
			FOperationFailure& OutFailure) const override { OutFailure = FOperationFailure(); return true; }
		virtual bool Rollback(FOperationFailure& OutFailure) override { OutFailure = FOperationFailure(); return true; }

	private:
		FString TargetKey;
		FReflectionRoot Root;
	};

	class FDirectLifecycle final : public IReflectionOperationLifecycle
	{
	public:
		explicit FDirectLifecycle(FReflectionRoot InRoot)
			: Root(MoveTemp(InRoot))
		{
		}

		virtual bool PrepareLifecycle(
			const FPrepareRequest& Request,
			TUniquePtr<IReflectionOperationLifecycleState>& OutState,
			FOperationFailure& OutFailure) const override
		{
			OutState.Reset();
			OutFailure = FOperationFailure();
			FString TargetKey;
			if (!Request.IsValid()
				|| Request.Input->RequestedOperation != EOperation::Update
				|| !Request.Input->Target.IsValid()
				|| !Request.Input->Target->TryGetStringField(TEXT("path"), TargetKey)
				|| TargetKey.IsEmpty()
				|| !Root.IsValid())
			{
				OutFailure.ErrorCode = TEXT("direct_reflection_lifecycle_invalid");
				OutFailure.ErrorDetail = OutFailure.ErrorCode;
				return false;
			}
			OutState = MakeUnique<FDirectLifecycleState>(MoveTemp(TargetKey), Root);
			return true;
		}

	private:
		FReflectionRoot Root;
	};

	bool ApplyPatch(
		const FReflectionRoot& Root,
		const TArray<TSharedPtr<FJsonValue>>& PropertyDescriptors,
		const UeAgentAuthoringObjectApply::FOptions& ObjectApplyOptions,
		UeAgentAuthoringDirectApply::FResult& OutResult)
	{
		using namespace UeAgentAuthoringDirectApply;
		OutResult = FResult();
		OutResult.Status = TEXT("failed");
		OutResult.PropertiesSeen = PropertyDescriptors.Num();
		if (!Root.IsValid())
		{
			OutResult.Phase = TEXT("preflight");
			OutResult.ErrorCode = TEXT("direct_reflection_root_invalid");
			OutResult.ErrorDetail = OutResult.ErrorCode;
			return false;
		}
		if (PropertyDescriptors.IsEmpty())
		{
			OutResult.Status = TEXT("applied");
			OutResult.Phase = TEXT("complete");
			return true;
		}
		TArray<TSharedPtr<FJsonValue>> ApplyDescriptors;
		ApplyDescriptors.Reserve(PropertyDescriptors.Num());
		for (const TSharedPtr<FJsonValue>& DescriptorValue : PropertyDescriptors)
		{
			const TSharedPtr<FJsonObject> ExportDescriptor =
				DescriptorValue.IsValid() && DescriptorValue->Type == EJson::Object
					? DescriptorValue->AsObject()
					: nullptr;
			const TSharedPtr<FJsonObject> ApplyDescriptor =
				UeAgentAuthoringObjectApply::ProjectDescriptorForApply(ExportDescriptor);
			if (!ApplyDescriptor.IsValid())
			{
				OutResult.Phase = TEXT("decode");
				OutResult.ErrorCode = TEXT("direct_property_descriptor_invalid");
				OutResult.ErrorDetail = OutResult.ErrorCode;
				return false;
			}
			ApplyDescriptors.Add(MakeShared<FJsonValueObject>(ApplyDescriptor));
		}

		const FContract InputContract = MakeInputContract();
		const UeAgentApplyWorkspaceContract::FContract WorkspaceContract = MakeWorkspaceContract();
		UeAgentApplyInputContract::FRegistry InputContracts;
		UeAgentApplyInputContract::ERegistrationResult InputRegistration;
		FString RegistrationError;
		if (!InputContracts.RegisterContract(InputContract, InputRegistration, RegistrationError))
		{
			OutResult.Phase = TEXT("preflight");
			OutResult.ErrorCode = RegistrationError;
			OutResult.ErrorDetail = RegistrationError;
			return false;
		}

		FProviderRegistry Providers;
		const TSharedRef<IReflectionOperationLifecycle> Lifecycle = MakeShared<FDirectLifecycle>(Root);
		const TSharedRef<IApplyProvider> Provider = MakeShared<FReflectionApplyProvider>(
			DirectProviderId,
			Lifecycle,
			ObjectApplyOptions);
		UeAgentAuthoringOperationApply::ERegistrationResult ProviderRegistration;
		if (!Providers.RegisterProvider(Provider, ProviderRegistration, RegistrationError))
		{
			OutResult.Phase = TEXT("preflight");
			OutResult.ErrorCode = RegistrationError;
			OutResult.ErrorDetail = RegistrationError;
			return false;
		}

		const FString TargetKey = Root.OwnerPath;
		TSharedRef<FJsonObject> Target = MakeShared<FJsonObject>();
		Target->SetStringField(TEXT("path"), TargetKey);
		TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
		Document->SetStringField(TEXT("operation"), TEXT("update"));
		Document->SetObjectField(TEXT("target"), Target);
		Document->SetArrayField(TEXT("properties"), ApplyDescriptors);

		UeAgentAuthoringWorkspaceRouter::FWorkspaceUnit Unit;
		Unit.UnitId = TEXT("primary");
		Unit.CanonicalUnitKey = TEXT("primary");
		Unit.RelativePath = TEXT("index.hjson");
		Unit.Role = TEXT("object");
		Unit.RouteId = TEXT("core.reflection_patch");
		Unit.ApplyContractRef = DirectInputContractId;
		Unit.MergePolicy = TEXT("replace");
		Unit.Presence = UeAgentAuthoringWorkspaceRouter::EUnitPresence::Present;
		Unit.Action = UeAgentAuthoringWorkspaceRouter::EUnitAction::Apply;
		Unit.Document = Document;
		Unit.Provenance.bVirtual = true;
		Unit.Provenance.SourceUnit = TEXT("primary");

		UeAgentAuthoringWorkspaceRouter::FWorkspace Workspace;
		Workspace.ContractId = WorkspaceContract.ContractId;
		Workspace.ContractVersion = WorkspaceContract.ContractVersion;
		Workspace.ProfileId = WorkspaceContract.ProfileId;
		Workspace.SourceKind = UeAgentApplyWorkspaceContract::ESourceKind::Inline;
		Workspace.WorkspaceRoot = TEXT("inline://core-reflection-patch");
		Workspace.Units = { Unit };
		UeAgentAuthoringWorkspaceDependencyGraph::FDependencyGraph Graph;
		UeAgentAuthoringWorkspaceDependencyGraph::FGraphFailure GraphFailure;
		if (!UeAgentAuthoringWorkspaceDependencyGraph::BuildDependencyGraph(
			WorkspaceContract,
			Workspace,
			UeAgentAuthoringWorkspaceDependencyGraph::FDependencyInputs(),
			Graph,
			GraphFailure))
		{
			OutResult.Phase = TEXT("preflight");
			OutResult.ErrorCode = GraphFailure.ErrorCode;
			OutResult.ErrorDetail = GraphFailure.ErrorDetail;
			return false;
		}

		FBackendOptions BackendOptions;
		BackendOptions.TransactionDescription = NSLOCTEXT(
			"UeAgentInterface", "DirectReflectionApply", "UAI Apply Structured Properties");
		FApplyWorkspaceResult ApplyResult;
		const bool bApplied = ApplyWorkspace(
			WorkspaceContract,
			Workspace,
			Graph,
			InputContracts,
			Providers,
			UeAgentAuthoringWorkspaceTransaction::FExecutionOptions(),
			BackendOptions,
			ApplyResult);
		if (!bApplied)
		{
			const bool bPreflightFailure = !ApplyResult.Preflight.IsReady();
			OutResult.Phase = bPreflightFailure ? TEXT("preflight") : ApplyResult.Execution.Phase;
			OutResult.ErrorCode = bPreflightFailure
				? ApplyResult.Preflight.CauseCode
				: ApplyResult.Execution.CauseCode;
			if (OutResult.ErrorCode.IsEmpty())
			{
				OutResult.ErrorCode = bPreflightFailure
					? ApplyResult.Preflight.ErrorCode
					: ApplyResult.Execution.ErrorCode;
			}
			OutResult.ErrorDetail = bPreflightFailure
				? ApplyResult.Preflight.ErrorDetail
				: ApplyResult.Execution.ErrorDetail;
			OutResult.PropertyPath = bPreflightFailure
				? ApplyResult.Preflight.Diagnostic.PropertyPath
				: ApplyResult.Execution.Diagnostic.PropertyPath;
			OutResult.bRollbackAttempted = ApplyResult.Execution.bRollbackAttempted;
			OutResult.bRollbackSucceeded = ApplyResult.Execution.bRollbackSucceeded;
			return false;
		}

		OutResult.Status = TEXT("applied");
		OutResult.Phase = TEXT("complete");
		return true;
	}
}

namespace UeAgentAuthoringDirectApply
{
	bool ApplyObjectPatch(
		UObject* Object,
		const TArray<TSharedPtr<FJsonValue>>& PropertyDescriptors,
		const UeAgentAuthoringObjectApply::FOptions& Options,
		FResult& OutResult)
	{
		return ApplyPatch(FReflectionRoot::MakeObject(Object), PropertyDescriptors, Options, OutResult);
	}

	bool ApplyStructPatch(
		UStruct* StructType,
		void* StructData,
		UObject* NotificationOwner,
		const TArray<TSharedPtr<FJsonValue>>& PropertyDescriptors,
		const UeAgentAuthoringObjectApply::FOptions& Options,
		FResult& OutResult)
	{
		return ApplyPatch(
			FReflectionRoot::MakeStruct(StructType, StructData, NotificationOwner),
			PropertyDescriptors,
			Options,
			OutResult);
	}
}

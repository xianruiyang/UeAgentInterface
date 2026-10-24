// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringOperationApply/UeAgentAuthoringDirectApply.h"

#include "Utilities/AuthoringOperationApply/UeAgentAuthoringObjectApplyProvider.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceDependencyGraph.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

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
		Result.PropertyPayload.Mode = TEXT("reflection");
		Result.PropertyPayload.PropertyPathField = TEXT("property_path");
		Result.PropertyPayload.ValueField = TEXT("value");
		Result.PropertyPayload.OptionalAssertionFields = {
			TEXT("kind"), TEXT("struct_path"), TEXT("enum_path"), TEXT("element_type"),
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
		FDirectLifecycleState(FString InTargetKey, FReflectionRoot InRoot,
			TArray<UeAgentAuthoringObjectApply::FPropertyIntent>& InIntent)
			: TargetKey(MoveTemp(InTargetKey))
			, Root(MoveTemp(InRoot))
			, Intent(InIntent)
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
		virtual void RetainPropertyIntent(const UeAgentAuthoringObjectApply::FPropertyIntent& Value) override { if (!Value.IsEmpty()) Intent.Add(Value); }

	private:
		FString TargetKey;
		FReflectionRoot Root;
		TArray<UeAgentAuthoringObjectApply::FPropertyIntent>& Intent;
	};

	class FDirectLifecycle final : public IReflectionOperationLifecycle
	{
	public:
		explicit FDirectLifecycle(FReflectionRoot InRoot,
			TArray<UeAgentAuthoringObjectApply::FPropertyIntent>& InIntent)
			: Root(MoveTemp(InRoot))
			, Intent(InIntent)
		{
		}

		virtual bool PrepareLifecycle(
			const FPrepareRequest& Request,
			TUniquePtr<IReflectionOperationLifecycleState>& OutState,
			FOperationFailure& OutFailure) const override
		{
			OutState.Reset();
			OutFailure = FOperationFailure();
			auto Fail = [&](const TCHAR* Reason, const TCHAR* Path)
			{
				OutFailure.Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(
					TEXT("direct_reflection_lifecycle_invalid"), Reason, TEXT("preflight"));
				OutFailure.Diagnostic.PropertyPath = Path;
				OutFailure.Diagnostic.TargetPath = Root.OwnerPath;
				return false;
			};
			if (!Request.Contract) return Fail(TEXT("Direct property apply preparation has no input contract."), TEXT("request.contract"));
			if (!Request.Unit) return Fail(TEXT("Direct property apply preparation has no workspace unit."), TEXT("request.unit"));
			if (!Request.Input) return Fail(TEXT("Direct property apply preparation has no prepared input."), TEXT("request.input"));
			if (Request.Input->RequestedOperation != EOperation::Update)
			{
				Fail(TEXT("Direct property apply only supports the update operation."), TEXT("operation"));
				OutFailure.Diagnostic.ActualValue = MakeShared<FJsonValueString>(OperationToString(Request.Input->RequestedOperation));
				OutFailure.Diagnostic.Source.JsonPath = TEXT("/operation");
				OutFailure.Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("update"));
				return false;
			}
			if (!Request.Input->Target.IsValid())
				return Fail(TEXT("Direct property apply requires a target object."), TEXT("target"));
			FString TargetKey;
			if (!Request.Input->Target->TryGetStringField(TEXT("path"), TargetKey) || TargetKey.IsEmpty())
			{
				Fail(TEXT("Direct property apply requires a non-empty target path string."), TEXT("target.path"));
				OutFailure.Diagnostic.Source.JsonPath = TEXT("/target/path");
				OutFailure.Diagnostic.ActualValue = Request.Input->Target->Values.FindRef(TEXT("path"));
				OutFailure.Diagnostic.ActualValuePresent = Request.Input->Target->HasField(TEXT("path"));
				return false;
			}
			if (!Root.IsValid())
				return Fail(TEXT("The object or struct receiving the property patch is unavailable."), TEXT("target"));
			OutState = MakeUnique<FDirectLifecycleState>(MoveTemp(TargetKey), Root, Intent);
			return true;
		}

	private:
		FReflectionRoot Root;
		TArray<UeAgentAuthoringObjectApply::FPropertyIntent>& Intent;
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
			OutResult.Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("direct_reflection_root_invalid"),
				TEXT("The object or struct receiving the property patch is unavailable."), TEXT("preflight"));
			OutResult.Diagnostic.TargetPath = Root.OwnerPath;
			return false;
		}
		if (PropertyDescriptors.IsEmpty())
		{
			OutResult.Status = TEXT("applied");
			OutResult.Phase = TEXT("complete");
			return true;
		}
		// Capture sources while descriptor identities still belong to the caller's
		// scope. The projection below keeps field paths and array order.
		auto OriginalDocument = MakeShared<FJsonObject>();
		OriginalDocument->SetArrayField(TEXT("properties"), PropertyDescriptors);
		auto OriginalSources = UeAgentAuthoringDiagnostics::FScopedSourceLocations::CaptureProjection(*OriginalDocument);
		TArray<TSharedPtr<FJsonValue>> ApplyDescriptors;
		ApplyDescriptors.Reserve(PropertyDescriptors.Num());
		for (int32 Index = 0; Index < PropertyDescriptors.Num(); ++Index)
		{
			const auto& DescriptorValue = PropertyDescriptors[Index];
			const TSharedPtr<FJsonObject> ExportDescriptor =
				DescriptorValue.IsValid() && DescriptorValue->Type == EJson::Object
					? DescriptorValue->AsObject()
					: nullptr;
			const TSharedPtr<FJsonObject> ApplyDescriptor =
				UeAgentAuthoringObjectApply::ProjectDescriptorForApply(ExportDescriptor);
			if (!ApplyDescriptor.IsValid())
			{
				OutResult.Phase = TEXT("decode");
				const FString Pointer = FString::Printf(TEXT("/properties/%d"), Index)
					+ (!ExportDescriptor ? TEXT("") : !ExportDescriptor->HasTypedField<EJson::String>(TEXT("property_path"))
						? TEXT("/property_path") : TEXT("/value"));
				OutResult.Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("direct_property_descriptor_invalid"),
					!ExportDescriptor ? TEXT("A property patch descriptor must be an object.")
						: !ExportDescriptor->HasTypedField<EJson::String>(TEXT("property_path"))
							? TEXT("A property patch descriptor requires a string property_path.")
							: TEXT("A property patch descriptor requires a value field."), TEXT("preflight"));
				OutResult.Diagnostic.Source = UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(OriginalSources, Pointer);
				if (OutResult.Diagnostic.Source.bCommandInput) OutResult.Diagnostic.SourceKind = TEXT("command_input");
				if (OutResult.Diagnostic.Source.JsonPath.IsEmpty() && OutResult.Diagnostic.Source.InternalJsonPath.IsEmpty())
					OutResult.Diagnostic.Source.JsonPath = Pointer;
				return false;
			}
			ApplyDescriptors.Add(MakeShared<FJsonValueObject>(ApplyDescriptor));
		}

		const FContract InputContract = MakeInputContract();
		const UeAgentApplyWorkspaceContract::FContract WorkspaceContract = MakeWorkspaceContract();
		UeAgentApplyInputContract::FRegistry InputContracts;
		UeAgentApplyInputContract::ERegistrationResult InputRegistration;
		UE::UAI::Contract::FDiagnostic RegistrationError;
		if (!InputContracts.RegisterContract(InputContract, InputRegistration, RegistrationError))
		{
			OutResult.Phase = TEXT("preflight");
			OutResult.Diagnostic = RegistrationError;
			return false;
		}

		FProviderRegistry Providers;
		const TSharedRef<IReflectionOperationLifecycle> Lifecycle = MakeShared<FDirectLifecycle>(Root, OutResult.PropertyIntents);
		const TSharedRef<IApplyProvider> Provider = MakeShared<FReflectionApplyProvider>(
			DirectProviderId,
			Lifecycle,
			ObjectApplyOptions);
		UeAgentAuthoringOperationApply::ERegistrationResult ProviderRegistration;
		if (!Providers.RegisterProvider(Provider, ProviderRegistration, RegistrationError))
		{
			OutResult.Phase = TEXT("preflight");
			OutResult.Diagnostic = RegistrationError;
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
		Unit.ResolveOriginalSource = [Sources = MoveTemp(OriginalSources)](const FString& Pointer)
			{ return UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(Sources, Pointer); };
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
			OutResult.Diagnostic = GraphFailure.Diagnostic;
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
			FApplyFailureProjection Failure;
			TryProjectApplyFailure(ApplyResult, Failure);
			OutResult.Phase = Failure.Phase;
			OutResult.Diagnostic = Failure.Diagnostic;
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

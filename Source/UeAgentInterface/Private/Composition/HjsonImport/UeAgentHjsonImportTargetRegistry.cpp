#include "Composition/HjsonImport/UeAgentHjsonImportTargetRegistry.h"

#include "Composition/GraphCompound/UeAgentGraphCompoundHjsonTargets.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "Core/Contract/UeAgentExecutionContextPolicy.h"
#include "DomainAdapters/TimelineAnimation/UeAgentTimelineAnimationHjsonTargets.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::Composition::HjsonImport
{
	using namespace UE::UAI::Contract;

	namespace
	{
		const FString& ImportInputSchema()
		{
			static const FString Schema = TEXT(
				"{\"additionalProperties\":false,\"properties\":{"
				"\"composition_kind\":{\"type\":\"string\"},"
				"\"dry_run\":{\"type\":\"boolean\"},"
				"\"expected_revision\":{\"type\":\"string\"},"
				"\"intent\":{\"type\":\"string\"},"
				"\"target\":{\"type\":\"string\"},"
				"\"workspace_fingerprint\":{\"type\":\"string\"}},"
				"\"required\":[\"composition_kind\",\"dry_run\","
				"\"expected_revision\",\"intent\",\"target\","
				"\"workspace_fingerprint\"],\"type\":\"object\"}");
			return Schema;
		}

		const FString& ImportResultSchema()
		{
			static const FString Schema = TEXT(
				"{\"additionalProperties\":true,\"properties\":{},"
				"\"type\":\"object\"}");
			return Schema;
		}

		FUeAgentDescriptorIdentity ImportIdentity(
			const FString& DescriptorId,
			const FString& CapabilityId)
		{
			TSharedPtr<FJsonObject> InputSchema;
			TSharedPtr<FJsonObject> ResultSchema;
			FString Error;
			if (!Json::TryParseObject(
					ImportInputSchema(), InputSchema, Error)
				|| !Json::TryParseObject(
					ImportResultSchema(), ResultSchema, Error))
			{
				return FUeAgentDescriptorIdentity(FString(), 0, FString());
			}

			TArray<TSharedPtr<FJsonValue>> StatePlanes;
			StatePlanes.Add(MakeShared<FJsonValueString>(TEXT("persisted")));
			const TSharedPtr<FJsonObject> Policy = MakeShared<FJsonObject>();
			Policy->SetArrayField(TEXT("allowed_state_planes"), MoveTemp(StatePlanes));
			Policy->SetArrayField(TEXT("required_facets"), {});
			Policy->SetStringField(TEXT("resolution_policy"), TEXT("explicit_only"));
			Policy->SetStringField(
				TEXT("transient_access_policy"),
				TEXT("temporary_mutation_allowed"));

			const TSharedPtr<FJsonObject> Descriptor = MakeShared<FJsonObject>();
			Descriptor->SetStringField(TEXT("capability_id"), CapabilityId);
			Descriptor->SetStringField(TEXT("descriptor_id"), DescriptorId);
			Descriptor->SetStringField(TEXT("domain_id"), TEXT("hjson_import"));
			Descriptor->SetObjectField(TEXT("execution_context_policy"), Policy);
			Descriptor->SetObjectField(TEXT("input_schema"), InputSchema);
			Descriptor->SetStringField(TEXT("operation_kind"), TEXT("operation"));
			Descriptor->SetObjectField(TEXT("result_schema"), ResultSchema);
			Descriptor->SetField(
				TEXT("version"), MakeShared<FJsonValueNumberString>(TEXT("1")));
			FString CanonicalDescriptor;
			if (!Json::TryCanonicalize(
					Descriptor, CanonicalDescriptor, Error))
			{
				return FUeAgentDescriptorIdentity(FString(), 0, FString());
			}
			return FUeAgentDescriptorIdentity(
				DescriptorId,
				1,
				Json::Sha256Utf8(CanonicalDescriptor));
		}

		void SetError(
			FUeAgentHjsonImportTargetError& OutError,
			FString Code,
			FString SubjectId = FString(),
			FString Detail = FString())
		{
			OutError = {};
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(SubjectId);
			OutError.Detail = MoveTemp(Detail);
		}

		bool TryCreateTimelineAnimationOperation(
			FUeAgentHjsonTargetImportRequest Request,
			TSharedPtr<IUeAgentHjsonImportOperation>& OutOperation,
			FUeAgentHjsonImportTargetError& OutError)
		{
			OutOperation.Reset();
			TSharedPtr<DomainAdapters::TimelineAnimation::
				FUeAgentTimelineAnimationHjsonOperation> Concrete;
			if (!DomainAdapters::TimelineAnimation::
				FUeAgentTimelineAnimationHjsonOperation::TryCreate(
					MoveTemp(Request), Concrete, OutError)
				|| !Concrete.IsValid())
			{
				return false;
			}
			OutOperation = Concrete;
			return true;
		}
	}

	bool FUeAgentHjsonImportTargetBinding::IsValid() const
	{
		return !CompositionKind.IsEmpty()
			&& ImportDescriptorIdentity.IsValid()
			&& !TypedOperationId.IsEmpty()
			&& !ProviderBinding.IsEmpty()
			&& !CompileSettlePolicy.IsEmpty()
			&& DurableKind == TEXT("package")
			&& OracleKind.StartsWith(TEXT("independent_unload_reload_"))
			&& !AllowedCapabilities.IsEmpty()
			&& HasOperationFactory();
	}

	bool FUeAgentHjsonImportTargetBinding::HasOperationFactory() const
	{
		return OperationFactory != nullptr;
	}

	bool FUeAgentHjsonImportTargetBinding::HasExportFactory() const
	{
		return ExportFactory != nullptr;
	}

	FUeAgentHjsonTargetExportRequest::FUeAgentHjsonTargetExportRequest(
		FString InCompositionKind,
		FString InTargetAssetPath,
		FString InProfileId,
		FString InWorkspaceRoot)
		: CompositionKind(MoveTemp(InCompositionKind))
		, TargetAssetPath(MoveTemp(InTargetAssetPath))
		, ProfileId(MoveTemp(InProfileId))
		, WorkspaceRoot(MoveTemp(InWorkspaceRoot))
	{
		CompositionKind.TrimStartAndEndInline();
		TargetAssetPath.TrimStartAndEndInline();
		ProfileId.TrimStartAndEndInline();
		WorkspaceRoot.TrimStartAndEndInline();
	}

	const FString& FUeAgentHjsonTargetExportRequest::GetCompositionKind() const { return CompositionKind; }
	const FString& FUeAgentHjsonTargetExportRequest::GetTargetAssetPath() const { return TargetAssetPath; }
	const FString& FUeAgentHjsonTargetExportRequest::GetProfileId() const { return ProfileId; }
	const FString& FUeAgentHjsonTargetExportRequest::GetWorkspaceRoot() const { return WorkspaceRoot; }
	bool FUeAgentHjsonTargetExportRequest::IsValid() const
	{
		return !CompositionKind.IsEmpty() && !TargetAssetPath.IsEmpty()
			&& !ProfileId.IsEmpty() && !WorkspaceRoot.IsEmpty();
	}

	FUeAgentHjsonTargetImportRequest::FUeAgentHjsonTargetImportRequest(
		FString InCompositionKind,
		FString InTargetAssetPath,
		HjsonWorkspace::FResolvedSemanticWorkspaceRef InWorkspace,
		PlanningPreflight::EChangeIntent InIntent,
		FString InExpectedBaseRevision,
		const bool bInDryRun)
		: CompositionKind(MoveTemp(InCompositionKind))
		, TargetAssetPath(MoveTemp(InTargetAssetPath))
		, Workspace(MoveTemp(InWorkspace))
		, Intent(InIntent)
		, ExpectedBaseRevision(MoveTemp(InExpectedBaseRevision))
		, bDryRun(bInDryRun)
	{
		CompositionKind.TrimStartAndEndInline();
		TargetAssetPath.TrimStartAndEndInline();
		ExpectedBaseRevision.TrimStartAndEndInline();
	}

	const FString& FUeAgentHjsonTargetImportRequest::GetCompositionKind() const { return CompositionKind; }
	const FString& FUeAgentHjsonTargetImportRequest::GetTargetAssetPath() const { return TargetAssetPath; }
	const HjsonWorkspace::FResolvedSemanticWorkspacePtr&
	FUeAgentHjsonTargetImportRequest::GetWorkspace() const { return Workspace; }
	PlanningPreflight::EChangeIntent FUeAgentHjsonTargetImportRequest::GetIntent() const { return Intent; }
	const FString& FUeAgentHjsonTargetImportRequest::GetExpectedBaseRevision() const { return ExpectedBaseRevision; }
	bool FUeAgentHjsonTargetImportRequest::IsDryRun() const { return bDryRun; }
	bool FUeAgentHjsonTargetImportRequest::IsValid() const
	{
		return !CompositionKind.IsEmpty() && !TargetAssetPath.IsEmpty()
			&& Workspace.IsValid() && Workspace->IsValid()
			&& Workspace->GetMode()
				== HjsonWorkspace::ESemanticWorkspaceMode::State
			&& !ExpectedBaseRevision.IsEmpty()
			&& (Intent == PlanningPreflight::EChangeIntent::Update
				|| Intent == PlanningPreflight::EChangeIntent::Replace);
	}

	FUeAgentHjsonTargetExportReceipt::FUeAgentHjsonTargetExportReceipt(
		FString InCompositionKind,
		FString InTargetAssetPath,
		FString InWorkspaceRoot,
		FString InBaseRevision,
		FString InWorkspaceFingerprint)
		: CompositionKind(MoveTemp(InCompositionKind))
		, TargetAssetPath(MoveTemp(InTargetAssetPath))
		, WorkspaceRoot(MoveTemp(InWorkspaceRoot))
		, BaseRevision(MoveTemp(InBaseRevision))
		, WorkspaceFingerprint(MoveTemp(InWorkspaceFingerprint))
	{
		bValid = !CompositionKind.IsEmpty() && !TargetAssetPath.IsEmpty()
			&& !WorkspaceRoot.IsEmpty() && !BaseRevision.IsEmpty()
			&& WorkspaceFingerprint.StartsWith(TEXT("sha256:"));
	}

	const FString& FUeAgentHjsonTargetExportReceipt::GetCompositionKind() const { return CompositionKind; }
	const FString& FUeAgentHjsonTargetExportReceipt::GetTargetAssetPath() const { return TargetAssetPath; }
	const FString& FUeAgentHjsonTargetExportReceipt::GetWorkspaceRoot() const { return WorkspaceRoot; }
	const FString& FUeAgentHjsonTargetExportReceipt::GetBaseRevision() const { return BaseRevision; }
	const FString& FUeAgentHjsonTargetExportReceipt::GetWorkspaceFingerprint() const { return WorkspaceFingerprint; }
	bool FUeAgentHjsonTargetExportReceipt::IsValid() const { return bValid; }

	FUeAgentHjsonTargetImportReceipt::FUeAgentHjsonTargetImportReceipt(
		FString InCompositionKind,
		FString InTargetAssetPath,
		FString InWorkspaceFingerprint,
		FString InPreRevision,
		FString InPersistedRevision,
		const bool bInMutationApplied,
		const bool bInCompileSettled,
		const bool bInPackageSaved,
		const bool bInUnloadedReloaded,
		TMap<FString, FString> InPersistedValues)
		: CompositionKind(MoveTemp(InCompositionKind))
		, TargetAssetPath(MoveTemp(InTargetAssetPath))
		, WorkspaceFingerprint(MoveTemp(InWorkspaceFingerprint))
		, PreRevision(MoveTemp(InPreRevision))
		, PersistedRevision(MoveTemp(InPersistedRevision))
		, bMutationApplied(bInMutationApplied)
		, bCompileSettled(bInCompileSettled)
		, bPackageSaved(bInPackageSaved)
		, bUnloadedReloaded(bInUnloadedReloaded)
		, PersistedValues(MoveTemp(InPersistedValues))
	{
		bValid = !CompositionKind.IsEmpty() && !TargetAssetPath.IsEmpty()
			&& WorkspaceFingerprint.StartsWith(TEXT("sha256:"))
			&& !PreRevision.IsEmpty() && !PersistedRevision.IsEmpty()
			&& (!bMutationApplied || (bCompileSettled && bPackageSaved
				&& bUnloadedReloaded && !PersistedValues.IsEmpty()));
	}

	const FString& FUeAgentHjsonTargetImportReceipt::GetWorkspaceFingerprint() const { return WorkspaceFingerprint; }
	const FString& FUeAgentHjsonTargetImportReceipt::GetPreRevision() const { return PreRevision; }
	const FString& FUeAgentHjsonTargetImportReceipt::GetPersistedRevision() const { return PersistedRevision; }
	bool FUeAgentHjsonTargetImportReceipt::WasMutationApplied() const { return bMutationApplied; }
	bool FUeAgentHjsonTargetImportReceipt::WasCompileSettled() const { return bCompileSettled; }
	bool FUeAgentHjsonTargetImportReceipt::WasPackageSaved() const { return bPackageSaved; }
	bool FUeAgentHjsonTargetImportReceipt::WasUnloadedReloaded() const { return bUnloadedReloaded; }
	FString FUeAgentHjsonTargetImportReceipt::GetPersistedValue(const FString& FieldId) const
	{
		const FString* Found = PersistedValues.Find(FieldId);
		return Found ? *Found : FString();
	}
	bool FUeAgentHjsonTargetImportReceipt::IsValid() const { return bValid; }

	FUeAgentHjsonImportTargetRegistry::FUeAgentHjsonImportTargetRegistry()
	{
		FUeAgentHjsonImportTargetBinding Blueprint;
		Blueprint.CompositionKind = TEXT("ue.blueprint.asset");
		Blueprint.ImportDescriptorIdentity = GetHjsonImportDescriptorIdentity(
			Blueprint.CompositionKind);
		Blueprint.TypedOperationId = TEXT("blueprint.authoring.apply_workspace");
		Blueprint.ProviderBinding = TEXT("graph_compound.hjson.blueprint");
		Blueprint.CompileSettlePolicy = TEXT("blueprint_compile_and_settle");
		Blueprint.DurableKind = TEXT("package");
		Blueprint.OracleKind =
			TEXT("independent_unload_reload_blueprint_readback");
		Blueprint.AllowedCapabilities = {
			TEXT("blueprint.authoring.apply"),
		};
		Blueprint.OperationFactory = &Composition::GraphCompound::
			TryCreateBlueprintHjsonImportOperation;
		Bindings.Add(MoveTemp(Blueprint));

		FUeAgentHjsonImportTargetBinding WidgetBlueprint;
		WidgetBlueprint.CompositionKind = TEXT("ue.widget_blueprint.asset");
		WidgetBlueprint.ImportDescriptorIdentity = GetHjsonImportDescriptorIdentity(
			WidgetBlueprint.CompositionKind);
		WidgetBlueprint.TypedOperationId =
			TEXT("widget_blueprint.authoring.apply_workspace");
		WidgetBlueprint.ProviderBinding =
			TEXT("graph_compound.hjson.widget_blueprint");
		WidgetBlueprint.CompileSettlePolicy = TEXT("blueprint_compile_and_settle");
		WidgetBlueprint.DurableKind = TEXT("package");
		WidgetBlueprint.OracleKind =
			TEXT("independent_unload_reload_widget_blueprint_readback");
		WidgetBlueprint.AllowedCapabilities = {
			TEXT("widget_blueprint.authoring.apply"),
		};
		WidgetBlueprint.OperationFactory = &Composition::GraphCompound::
			TryCreateWidgetBlueprintHjsonImportOperation;
		Bindings.Add(MoveTemp(WidgetBlueprint));

		FUeAgentHjsonImportTargetBinding NiagaraSystem;
		NiagaraSystem.CompositionKind = TEXT("ue.niagara_system.asset");
		NiagaraSystem.ImportDescriptorIdentity =
			GetHjsonImportDescriptorIdentity(NiagaraSystem.CompositionKind);
		NiagaraSystem.TypedOperationId =
			TEXT("niagara.system.authoring.apply_workspace");
		NiagaraSystem.ProviderBinding =
			TEXT("graph_compound.hjson.niagara_system");
		NiagaraSystem.CompileSettlePolicy =
			TEXT("niagara_compile_refresh_and_settle");
		NiagaraSystem.DurableKind = TEXT("package");
		NiagaraSystem.OracleKind =
			TEXT("independent_unload_reload_niagara_system_readback");
		NiagaraSystem.AllowedCapabilities = {
			TEXT("niagara.stack.apply"),
			TEXT("niagara.compile_refresh_settle"),
		};
		NiagaraSystem.OperationFactory = &Composition::GraphCompound::
			TryCreateNiagaraSystemHjsonImportOperation;
		Bindings.Add(MoveTemp(NiagaraSystem));

		FUeAgentHjsonImportTargetBinding NiagaraEmitter;
		NiagaraEmitter.CompositionKind = TEXT("ue.niagara_emitter.asset");
		NiagaraEmitter.ImportDescriptorIdentity =
			GetHjsonImportDescriptorIdentity(NiagaraEmitter.CompositionKind);
		NiagaraEmitter.TypedOperationId =
			TEXT("niagara.emitter.authoring.apply_workspace");
		NiagaraEmitter.ProviderBinding =
			TEXT("graph_compound.hjson.niagara_emitter");
		NiagaraEmitter.CompileSettlePolicy =
			TEXT("niagara_compile_refresh_and_settle");
		NiagaraEmitter.DurableKind = TEXT("package");
		NiagaraEmitter.OracleKind =
			TEXT("independent_unload_reload_niagara_emitter_readback");
		NiagaraEmitter.AllowedCapabilities = {
			TEXT("niagara.emitter.stack.apply"),
			TEXT("niagara.compile_refresh_settle"),
		};
		NiagaraEmitter.OperationFactory = &Composition::GraphCompound::
			TryCreateNiagaraEmitterHjsonImportOperation;
		Bindings.Add(MoveTemp(NiagaraEmitter));

		FUeAgentHjsonImportTargetBinding NiagaraScript;
		NiagaraScript.CompositionKind = TEXT("ue.niagara_script.asset");
		NiagaraScript.ImportDescriptorIdentity =
			GetHjsonImportDescriptorIdentity(NiagaraScript.CompositionKind);
		NiagaraScript.TypedOperationId =
			TEXT("niagara.script.authoring.apply_workspace");
		NiagaraScript.ProviderBinding =
			TEXT("graph_compound.hjson.niagara_script");
		NiagaraScript.CompileSettlePolicy =
			TEXT("niagara_compile_refresh_and_settle");
		NiagaraScript.DurableKind = TEXT("package");
		NiagaraScript.OracleKind =
			TEXT("independent_unload_reload_niagara_script_readback");
		NiagaraScript.AllowedCapabilities = {
			TEXT("niagara.script.stack.apply"),
			TEXT("niagara.compile_refresh_settle"),
		};
		NiagaraScript.OperationFactory = &Composition::GraphCompound::
			TryCreateNiagaraScriptHjsonImportOperation;
		Bindings.Add(MoveTemp(NiagaraScript));

		FUeAgentHjsonImportTargetBinding Animation;
		Animation.CompositionKind = TEXT("ue.animation_sequence.asset");
		Animation.ImportDescriptorIdentity = GetHjsonImportDescriptorIdentity(
			Animation.CompositionKind);
		Animation.TypedOperationId =
			TEXT("timeline_animation.animation_sequence.apply_workspace");
		Animation.ProviderBinding =
			TEXT("timeline_animation.hjson.animation_sequence");
		Animation.CompileSettlePolicy =
			TEXT("animation_sequence_rebuild_notify_compress_settle");
		Animation.DurableKind = TEXT("package");
		Animation.OracleKind =
			TEXT("independent_unload_reload_animation_sequence_readback");
		Animation.AllowedCapabilities = {
			TEXT("animation.sequence.bone_tracks.replace"),
			TEXT("animation.sequence.curves.update"),
			TEXT("animation.sequence.metadata.update"),
			TEXT("animation.sequence.events.update"),
		};
		Animation.OperationFactory = &TryCreateTimelineAnimationOperation;
		Animation.ExportFactory = &DomainAdapters::TimelineAnimation::
			TryExportTimelineAnimationHjsonTarget;
		Bindings.Add(MoveTemp(Animation));

		FUeAgentHjsonImportTargetBinding Level;
		Level.CompositionKind = TEXT("ue.level_sequence.asset");
		Level.ImportDescriptorIdentity = GetHjsonImportDescriptorIdentity(
			Level.CompositionKind);
		Level.TypedOperationId =
			TEXT("timeline_animation.level_sequence.apply_workspace");
		Level.ProviderBinding =
			TEXT("timeline_animation.hjson.level_sequence");
		Level.CompileSettlePolicy =
			TEXT("movie_scene_compile_and_evaluation_template_settle");
		Level.DurableKind = TEXT("package");
		Level.OracleKind =
			TEXT("independent_unload_reload_level_sequence_readback");
		Level.AllowedCapabilities = {
			TEXT("timeline.sequence.structure.replace"),
		};
		Level.OperationFactory = &TryCreateTimelineAnimationOperation;
		Level.ExportFactory = &DomainAdapters::TimelineAnimation::
			TryExportTimelineAnimationHjsonTarget;
		Bindings.Add(MoveTemp(Level));
	}

	const TArray<FUeAgentHjsonImportTargetBinding>&
	FUeAgentHjsonImportTargetRegistry::GetBindings() const { return Bindings; }

	const FUeAgentHjsonImportTargetBinding*
	FUeAgentHjsonImportTargetRegistry::FindExact(const FString& CompositionKind) const
	{
		return Bindings.FindByPredicate(
			[&CompositionKind](const FUeAgentHjsonImportTargetBinding& Binding)
			{
				return Binding.CompositionKind == CompositionKind;
			});
	}

	bool FUeAgentHjsonImportTargetRegistry::TryExportExact(
		const FUeAgentHjsonTargetExportRequest& Request,
		FUeAgentHjsonTargetExportReceipt& OutReceipt,
		FUeAgentHjsonImportTargetError& OutError) const
	{
		OutReceipt = {};
		OutError = {};
		if (!Request.IsValid())
		{
			SetError(OutError, TEXT("hjson_target_export_request_invalid"));
			return false;
		}
		const FUeAgentHjsonImportTargetBinding* Binding =
			FindExact(Request.GetCompositionKind());
		if (!Binding)
		{
			SetError(
				OutError,
				TEXT("hjson_import_target_not_found"),
				Request.GetCompositionKind());
			return false;
		}
		if (!Binding->HasExportFactory())
		{
			SetError(
				OutError,
				TEXT("hjson_target_export_not_supported"),
				Request.GetCompositionKind());
			return false;
		}
		return Binding->ExportFactory(Request, OutReceipt, OutError);
	}

	bool FUeAgentHjsonImportTargetRegistry::TryCreateOperationExact(
		FUeAgentHjsonTargetImportRequest Request,
		TSharedPtr<IUeAgentHjsonImportOperation>& OutOperation,
		FUeAgentHjsonImportTargetError& OutError) const
	{
		OutOperation.Reset();
		OutError = {};
		if (!Request.IsValid())
		{
			SetError(OutError, TEXT("hjson_target_import_request_invalid"));
			return false;
		}
		const FString CompositionKind = Request.GetCompositionKind();
		const FUeAgentHjsonImportTargetBinding* Binding =
			FindExact(CompositionKind);
		if (!Binding || !Binding->HasOperationFactory())
		{
			SetError(
				OutError,
				TEXT("hjson_import_target_not_found"),
				CompositionKind);
			return false;
		}
		if (!Binding->OperationFactory(
				MoveTemp(Request), OutOperation, OutError)
			|| !OutOperation.IsValid())
		{
			if (!OutError.IsValid())
			{
				SetError(
					OutError,
					TEXT("hjson_import_operation_factory_failed"),
					CompositionKind);
			}
			return false;
		}
		return true;
	}

	const FUeAgentHjsonImportTargetRegistry& GetHjsonImportTargetRegistry()
	{
		static const FUeAgentHjsonImportTargetRegistry Registry;
		return Registry;
	}

	FUeAgentDescriptorIdentity GetHjsonImportDescriptorIdentity(
		const FString& CompositionKind)
	{
		if (CompositionKind == TEXT("ue.blueprint.asset"))
		{
			return ImportIdentity(
				TEXT("uai.operation.blueprint.authoring_import"),
				TEXT("blueprint.authoring.apply"));
		}
		if (CompositionKind == TEXT("ue.widget_blueprint.asset"))
		{
			return ImportIdentity(
				TEXT("uai.operation.widget_blueprint.authoring_import"),
				TEXT("widget_blueprint.authoring.apply"));
		}
		if (CompositionKind == TEXT("ue.niagara_system.asset"))
		{
			return ImportIdentity(
				TEXT("uai.operation.niagara.system.authoring_import"),
				TEXT("niagara.stack.apply"));
		}
		if (CompositionKind == TEXT("ue.niagara_emitter.asset"))
		{
			return ImportIdentity(
				TEXT("uai.operation.niagara.emitter.authoring_import"),
				TEXT("niagara.emitter.stack.apply"));
		}
		if (CompositionKind == TEXT("ue.niagara_script.asset"))
		{
			return ImportIdentity(
				TEXT("uai.operation.niagara.script.authoring_import"),
				TEXT("niagara.script.stack.apply"));
		}
		if (CompositionKind == TEXT("ue.animation_sequence.asset"))
		{
			return ImportIdentity(
				TEXT("uai.operation.animation.sequence.authoring_import"),
				TEXT("animation.sequence.authoring_import"));
		}
		if (CompositionKind == TEXT("ue.level_sequence.asset"))
		{
			return ImportIdentity(
				TEXT("uai.operation.timeline.sequence.authoring_import"),
				TEXT("timeline.sequence.authoring_import"));
		}
		return FUeAgentDescriptorIdentity(FString(), 0, FString());
	}

	bool TryCreateHjsonImportDescriptorRegistry(
		TSharedPtr<const FUeAgentDescriptorRegistry>& OutRegistry,
		FString& OutError)
	{
		OutRegistry.Reset();
		OutError.Reset();
		const FUeAgentExecutionContextPolicy Policy(
			{EUeAgentStatePlane::Persisted},
			{},
			EUeAgentContextResolutionPolicy::ExplicitOnly,
			EUeAgentTransientContextPolicy::TemporaryMutationAllowed);
		TArray<FUeAgentOperationDescriptor> Descriptors;
		Descriptors.Emplace(
			GetHjsonImportDescriptorIdentity(TEXT("ue.blueprint.asset")),
			TEXT("blueprint.authoring.apply"),
			TEXT("hjson_import"),
			EUeAgentDescriptorOperationKind::Operation,
			Policy,
			FUeAgentContractValue(ImportInputSchema()),
			FUeAgentContractValue(ImportResultSchema()));
		Descriptors.Emplace(
			GetHjsonImportDescriptorIdentity(TEXT("ue.widget_blueprint.asset")),
			TEXT("widget_blueprint.authoring.apply"),
			TEXT("hjson_import"),
			EUeAgentDescriptorOperationKind::Operation,
			Policy,
			FUeAgentContractValue(ImportInputSchema()),
			FUeAgentContractValue(ImportResultSchema()));
		Descriptors.Emplace(
			GetHjsonImportDescriptorIdentity(TEXT("ue.niagara_system.asset")),
			TEXT("niagara.stack.apply"),
			TEXT("hjson_import"),
			EUeAgentDescriptorOperationKind::Operation,
			Policy,
			FUeAgentContractValue(ImportInputSchema()),
			FUeAgentContractValue(ImportResultSchema()));
		Descriptors.Emplace(
			GetHjsonImportDescriptorIdentity(TEXT("ue.niagara_emitter.asset")),
			TEXT("niagara.emitter.stack.apply"),
			TEXT("hjson_import"),
			EUeAgentDescriptorOperationKind::Operation,
			Policy,
			FUeAgentContractValue(ImportInputSchema()),
			FUeAgentContractValue(ImportResultSchema()));
		Descriptors.Emplace(
			GetHjsonImportDescriptorIdentity(TEXT("ue.niagara_script.asset")),
			TEXT("niagara.script.stack.apply"),
			TEXT("hjson_import"),
			EUeAgentDescriptorOperationKind::Operation,
			Policy,
			FUeAgentContractValue(ImportInputSchema()),
			FUeAgentContractValue(ImportResultSchema()));
		Descriptors.Emplace(
			GetHjsonImportDescriptorIdentity(
				TEXT("ue.animation_sequence.asset")),
			TEXT("animation.sequence.authoring_import"),
			TEXT("hjson_import"),
			EUeAgentDescriptorOperationKind::Operation,
			Policy,
			FUeAgentContractValue(ImportInputSchema()),
			FUeAgentContractValue(ImportResultSchema()));
		Descriptors.Emplace(
			GetHjsonImportDescriptorIdentity(
				TEXT("ue.level_sequence.asset")),
			TEXT("timeline.sequence.authoring_import"),
			TEXT("hjson_import"),
			EUeAgentDescriptorOperationKind::Operation,
			Policy,
			FUeAgentContractValue(ImportInputSchema()),
			FUeAgentContractValue(ImportResultSchema()));
		return FUeAgentDescriptorRegistry::TryCreate(
			MoveTemp(Descriptors), OutRegistry, OutError);
	}
}

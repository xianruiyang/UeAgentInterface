#include "Domains/Deformer/Authoring/UeAgentDeformerAuthoring.h"

#include "Composition/GraphCompound/UeAgentGraphCompoundDeformerProduction.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundPcgAnimationRoutes.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "OptimusDeformer.h"
#include "OptimusNode.h"
#include "OptimusNodeGraph.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticRegistry.h"
#include "Utilities/AuthoringExport/UeAgentCompoundProjectionExtension.h"
#include "Utilities/AuthoringExport/UeAgentExportContract.h"

namespace UeAgentDeformerAuthoring
{
	namespace
	{
		using namespace UE::UAI::Composition::GraphCompound;

		bool RegisterIdentity(const TCHAR* ModelId, const TCHAR* Kind,
			const TArray<FString>& Selectors, UE::UAI::Contract::FDiagnostic& OutError)
		{
			UeAgentAuthoringIdentity::FModelDescriptor Model;
			Model.ModelId = ModelId;
			Model.TargetKind = Kind;
			for (int32 Index = 0; Index < Selectors.Num(); ++Index)
				Model.Selectors.Add({Selectors[Index], 100 + Index, false});
			Model.KeyPatterns = {{TEXT("scoped"), 100, Selectors}};
			UeAgentAuthoringIdentity::ERegistrationResult Result;
			return UeAgentAuthoringIdentity::FRegistry::Get().RegisterModel(
				Model, Result, OutError);
		}

		bool RegisterSemantic(const TCHAR* SemanticId, const TCHAR* Kind,
			const TCHAR* ModelId,
			UeAgentAuthoringSemanticRegistry::EWriteCapability Write,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			UeAgentAuthoringSemanticRegistry::FSemanticDescriptor Semantic;
			Semantic.SemanticId = SemanticId;
			Semantic.DomainId = DomainId;
			Semantic.TargetKind = Kind;
			Semantic.ValueType = Write == UeAgentAuthoringSemanticRegistry::EWriteCapability::PropertyAssignment ? TEXT("*") : TEXT("object");
			Semantic.OwnerModel = TEXT("deformer_graph_owner");
			Semantic.IdentityModel = ModelId;
			Semantic.StateAccessor = Write == UeAgentAuthoringSemanticRegistry::EWriteCapability::PropertyAssignment ? TEXT("core.property") : StateAccessorId;
			Semantic.WriteCapability = Write;
			Semantic.ConstraintSource = TEXT("optimus.graph.schema");
			Semantic.AvailabilitySource = StateAccessorId;
			Semantic.Comparator = Write == UeAgentAuthoringSemanticRegistry::EWriteCapability::PropertyAssignment
				? TEXT("uai.compare.canonical_value") : TEXT("uai.compare.graph_vector_small_movement");
			Semantic.CapabilityTags = {TEXT("deformer"), TEXT("optimus"), TEXT("graph")};
			UeAgentAuthoringSemanticRegistry::ERegistrationResult Result;
			return UeAgentAuthoringSemanticRegistry::FRegistry::Get().RegisterSemantic(
				Semantic, Result, OutError);
		}

		bool AddRequest(const TCHAR* SemanticId, const TCHAR* ModelId,
			const TCHAR* Kind, const TMap<FString, FString>& Selectors,
			UOptimusDeformer& Asset,
			TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			UeAgentAuthoringState::FReadRequest Request;
			Request.SemanticId = SemanticId;
			if (!UeAgentAuthoringIdentity::BuildCanonicalIdentity(
					ModelId, Kind, Selectors, Request.TargetIdentity, OutError)) return false;
			Request.TargetQuery.ModelId = Request.TargetIdentity.ModelId;
			Request.TargetQuery.TargetKind = Request.TargetIdentity.TargetKind;
			Request.TargetQuery.CanonicalKey = Request.TargetIdentity.CanonicalKey;
			Request.RootObject = &Asset;
			Request.RootOwnerPath = Request.TargetIdentity.GetSelector(TEXT("asset_path"));
			OutRequests.Add(MoveTemp(Request));
			return true;
		}

		UeAgentAuthoringState::EReadOutcome ReadState(
			const UeAgentAuthoringState::FReadRequest& Request,
			UeAgentAuthoringState::FStateValue& OutValue, UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutValue = {};
			OutValue.TargetIdentity = Request.TargetIdentity;
			OutValue.Type.Kind = TEXT("object");
			OutValue.Provenance = StateAccessorId;
			UOptimusDeformer* Asset = const_cast<UOptimusDeformer*>(
				Cast<UOptimusDeformer>(Request.RootObject));
			if (!Asset)
			{
				Asset = LoadObject<UOptimusDeformer>(nullptr,
					*Request.TargetIdentity.GetSelector(TEXT("asset_path")));
			}
			if (!Asset)
			{
				OutValue.AvailabilityReason = TEXT("target_not_found");
				return UeAgentAuthoringState::EReadOutcome::Unavailable;
			}
			TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
			if (Request.SemanticId == GraphSemanticId)
			{
				Value->SetStringField(TEXT("asset_path"), Request.TargetIdentity.GetSelector(TEXT("asset_path")));
				Value->SetStringField(TEXT("asset_kind"), TEXT("deformer_graph"));
				Value->SetStringField(TEXT("graph_name"), TEXT("UpdateGraph"));
			}
			else if (Request.SemanticId == ExportTargetSemanticId)
			{
				Value->SetStringField(TEXT("asset_path"), Request.TargetIdentity.GetSelector(TEXT("asset_path")));
				Value->SetStringField(TEXT("asset_kind"), TEXT("deformer_graph"));
			}
			else
			{
				const TArray<FPcgAnimationGraphRouteProfile>* Profiles = nullptr;
				if (!TryGetPcgAnimationGraphRouteProfiles(Profiles, OutError)) return UeAgentAuthoringState::EReadOutcome::Failed;
				const FPcgAnimationGraphRouteProfile* Profile = Profiles->FindByPredicate(
					[](const FPcgAnimationGraphRouteProfile& Candidate)
					{
						return Candidate.GetCapabilityId() == TEXT("animation.deformer_graph.topology.apply");
					});
				TArray<FGraphCompoundDeformerCapturedValue> Captured;
				FGraphCompoundRuntimeError CaptureError;
				if (!Profile)
				{
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("deformer_capture_failed"),
						TEXT("The Deformer topology route required for state capture is unavailable."), TEXT("authoring_state"));
					OutError.TargetPath = Request.TargetIdentity.GetSelector(TEXT("asset_path"));
					OutError.SourceKind = TEXT("native_result");
					return UeAgentAuthoringState::EReadOutcome::Failed;
				}
				if (!TryCaptureDeformerValues(*Profile, *Asset, Captured, CaptureError, true,
					Request.TargetIdentity.GetSelector(TEXT("asset_path"))))
				{
					OutError = MoveTemp(CaptureError);
					return UeAgentAuthoringState::EReadOutcome::Failed;
				}
				const FString Key = Request.TargetIdentity.GetSelector(TEXT("element_ref"));
				const FGraphCompoundDeformerCapturedValue* Found = Captured.FindByPredicate(
					[&Key](const FGraphCompoundDeformerCapturedValue& Entry)
					{
						return Entry.SemanticId == Key;
					});
				if (!Found)
				{
					const FString NodeName = Request.TargetIdentity.GetSelector(TEXT("node_id"));
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("deformer_capture_element_missing"),
						NodeName.IsEmpty() ? TEXT("The graph element required by state readback is absent from the captured Deformer topology.")
							: FString::Printf(TEXT("Node '%s' is absent from the captured Deformer topology."), *NodeName), TEXT("authoring_state"));
					OutError.TargetPath = Request.TargetIdentity.GetSelector(TEXT("asset_path"));
					OutError.SourceKind = TEXT("native_result");
					OutError.SubjectId = Key;
					OutError.ActualValue = MakeShared<FJsonValueString>(Request.TargetIdentity.TargetKind);
					OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("matching captured graph element"));
					return UeAgentAuthoringState::EReadOutcome::Failed;
				}
				Value = Found->Value;
			}
			OutValue.Value = MakeShared<FJsonValueObject>(Value);
			OutValue.bAvailable = true;
			return UeAgentAuthoringState::EReadOutcome::Resolved;
		}

		UeAgentExportContract::FFieldDescriptor Field(const TCHAR* Id,
			const TCHAR* Path, UeAgentExportContract::EFieldSourceKind SourceKind,
			const TCHAR* SemanticId, const TCHAR* Projector, bool bEditable)
		{
			UeAgentExportContract::FFieldDescriptor Result;
			Result.FieldId = Id; Result.OutputPath = Path; Result.Source.Kind = SourceKind;
			Result.Source.SemanticId = SemanticId; Result.Source.ProjectorId = Projector;
			Result.Role = bEditable ? TEXT("authoring") : TEXT("identity");
			Result.bEditable = bEditable; Result.bRequired = true;
			Result.Representation = TEXT("canonical"); Result.EmptyPolicy = TEXT("emit_empty");
			Result.DefaultPolicy = TEXT("emit"); Result.Externalization = TEXT("inline");
			return Result;
		}

		bool Project(const UeAgentCompoundProjection::FRequest& Request,
			TArray<UeAgentCompoundProjection::FExpansionSelection>& Out,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			Out.Reset(); OutError.Reset();
			if (!Request.IsValid() || Request.WildcardCount != 1)
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("deformer_projection_request_invalid"),
					!Request.Contract ? TEXT("The Deformer projection request has no export contract.")
						: !Request.Profile ? TEXT("The Deformer projection request has no profile descriptor.")
						: !Request.File ? TEXT("The Deformer projection request has no file descriptor.")
						: !Request.Field ? TEXT("The Deformer projection request has no field descriptor.")
						: TEXT("Deformer element projection requires exactly one wildcard."), TEXT("authoring_state"));
				OutError.SourceKind = TEXT("native_result");
				OutError.PropertyPath = Request.JsonPointerPattern;
				if (Request.Contract && Request.Profile && Request.File && Request.Field)
				{
					OutError.ActualValue = MakeShared<FJsonValueNumber>(Request.WildcardCount);
					OutError.ExpectedValue = MakeShared<FJsonValueNumber>(1);
				}
				return false;
			}
			for (int32 Index = 0; Index < Request.Entries.Num(); ++Index)
			{
				const FString Key = Request.Entries[Index].TargetIdentity.GetSelector(TEXT("element_ref"));
				if (Key.IsEmpty())
				{
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("deformer_projection_identity_missing"),
						FString::Printf(TEXT("Projection entry %d has no native element reference."), Index), TEXT("authoring_state"));
					OutError.SourceKind = TEXT("native_result");
					OutError.TargetPath = Request.Entries[Index].TargetIdentity.GetSelector(TEXT("asset_path"));
					OutError.PropertyPath = FString::Printf(TEXT("/entries/%d/element_ref"), Index);
					OutError.ActualValue = MakeShared<FJsonValueString>(Key);
					OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-empty native element reference"));
					return false;
				}
				UeAgentCompoundProjection::FExpansionSelection Selection;
				Selection.WildcardValues = {Key}; Selection.EntryIndices = {Index};
				Out.Add(MoveTemp(Selection));
			}
			return true;
		}
	}

	bool BuildExportSnapshotReadRequests(UOptimusDeformer& Asset,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests, UE::UAI::Contract::FDiagnostic& OutError,
		const FString& IdentityAssetPath, const FString& IdentityObjectPath)
	{
		OutRequests.Reset(); OutError.Reset();
		const FString Path = IdentityAssetPath.IsEmpty() ? Asset.GetOutermost()->GetName() : IdentityAssetPath;
		UeAgentAuthoringState::FEditablePropertyReadOptions RootOptions;
		RootOptions.SemanticId = TEXT("core.property");
		RootOptions.BuildSnapshotIdentity = [OwnerPath = IdentityObjectPath.IsEmpty() ? Asset.GetPathName() : IdentityObjectPath](const FString& PropertyPath,
			UeAgentAuthoringIdentity::FCanonicalIdentity& Identity, UE::UAI::Contract::FDiagnostic& Error)
		{
			return UeAgentAuthoringIdentity::BuildCanonicalIdentity(TEXT("property_identity"), TEXT("property"),
				{{TEXT("owner_path"), OwnerPath}, {TEXT("property_path"), PropertyPath}}, Identity, Error);
		};
		if (!UeAgentAuthoringState::AppendEditableObjectPropertyReadRequests(&Asset, RootOptions, OutRequests, OutError)) return false;
		if (!AddRequest(GraphSemanticId, GraphIdentityModelId, TargetKind,
			{{TEXT("asset_path"), Path}}, Asset, OutRequests, OutError)
			|| !AddRequest(ExportTargetSemanticId, GraphIdentityModelId, TargetKind,
				{{TEXT("asset_path"), Path}}, Asset, OutRequests, OutError)) return false;
		const TArray<FPcgAnimationGraphRouteProfile>* Profiles = nullptr;
		if (!TryGetPcgAnimationGraphRouteProfiles(Profiles, OutError)) return false;
		const FPcgAnimationGraphRouteProfile* Profile = Profiles->FindByPredicate(
			[](const FPcgAnimationGraphRouteProfile& Candidate)
			{
				return Candidate.GetCapabilityId() == TEXT("animation.deformer_graph.topology.apply");
			});
		TArray<FGraphCompoundDeformerCapturedValue> Captured;
		FGraphCompoundRuntimeError CaptureError;
		if (!Profile)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("deformer_capture_failed"),
				TEXT("The Deformer topology route required to build snapshot reads is unavailable."), TEXT("authoring_state"));
			OutError.TargetPath = Path;
			OutError.SourceKind = TEXT("native_result");
			return false;
		}
		if (!TryCaptureDeformerValues(*Profile, Asset, Captured, CaptureError, true,
			IdentityObjectPath.IsEmpty() ? Path : IdentityObjectPath))
		{
			OutError = MoveTemp(CaptureError);
			return false;
		}
		for (const FGraphCompoundDeformerCapturedValue& Entry : Captured)
		{
			const bool bEdge = Entry.SemanticId.StartsWith(TEXT("relation:"));
			TMap<FString, FString> Selectors{{TEXT("asset_path"), Path}, {TEXT("element_ref"), Entry.SemanticId}};
			if (!bEdge) Selectors.Add(TEXT("node_id"), Entry.Value->GetStringField(TEXT("node_name")));
			if (!AddRequest(bEdge ? EdgeSemanticId : NodeSemanticId,
				bEdge ? EdgeIdentityModelId : NodeIdentityModelId,
				bEdge ? TEXT("deformer_graph_edge") : TEXT("deformer_graph_node"),
				Selectors,
				Asset, OutRequests, OutError)) return false;
		}
		if (const UOptimusNodeGraph* Graph = Asset.GetUpdateGraph())
		for (const UOptimusNode* Node : Graph->GetAllNodes())
		{
			if (!Node) continue;
			UeAgentAuthoringState::FEditablePropertyReadOptions Options;
			Options.SemanticId = NodePropertySemanticId;
			Options.BuildSnapshotIdentity = [Path, NodeId = Node->GetName()](const FString& PropertyPath,
				UeAgentAuthoringIdentity::FCanonicalIdentity& Identity, UE::UAI::Contract::FDiagnostic& Error)
			{
				return UeAgentAuthoringIdentity::BuildCanonicalIdentity(NodePropertyIdentityModelId, TEXT("deformer_node_property"),
					{{TEXT("asset_path"), Path}, {TEXT("node_id"), NodeId}, {TEXT("property_path"), PropertyPath}}, Identity, Error);
			};
			if (!UeAgentAuthoringState::AppendEditableObjectPropertyReadRequests(Node, Options, OutRequests, OutError)) return false;
		}
		return true;
	}

	bool RegisterAuthoring(UE::UAI::Contract::FDiagnostic& OutError)
	{
		static bool bRegistered = false;
		if (bRegistered) { OutError.Reset(); return true; }
		if (!RegisterIdentity(GraphIdentityModelId, TargetKind, {TEXT("asset_path")}, OutError)
			|| !RegisterIdentity(NodeIdentityModelId, TEXT("deformer_graph_node"),
				{TEXT("asset_path"), TEXT("element_ref"), TEXT("node_id")}, OutError)
			|| !RegisterIdentity(NodePropertyIdentityModelId, TEXT("deformer_node_property"),
				{TEXT("asset_path"), TEXT("node_id"), TEXT("property_path")}, OutError)
			|| !RegisterIdentity(EdgeIdentityModelId, TEXT("deformer_graph_edge"),
				{TEXT("asset_path"), TEXT("element_ref")}, OutError)) return false;
		UeAgentAuthoringSemanticRegistry::FDomainDescriptor Domain;
		Domain.DomainId = DomainId; Domain.MutationProviderId = TEXT("deformer.graph.typed_adapter");
		Domain.TargetKinds = {TargetKind, TEXT("deformer_graph_node"), TEXT("deformer_graph_edge"), TEXT("deformer_node_property")};
		Domain.CapabilityTags = {TEXT("graph"), TEXT("node"), TEXT("pin"), TEXT("connection")};
		UeAgentAuthoringSemanticRegistry::ERegistrationResult DomainResult;
		if (!UeAgentAuthoringSemanticRegistry::FRegistry::Get().RegisterDomain(
			Domain, DomainResult, OutError)) return false;
		using EWrite = UeAgentAuthoringSemanticRegistry::EWriteCapability;
		if (!RegisterSemantic(GraphSemanticId, TargetKind, GraphIdentityModelId, EWrite::DomainMutation, OutError)
			|| !RegisterSemantic(ExportTargetSemanticId, TargetKind, GraphIdentityModelId, EWrite::ReadOnly, OutError)
			|| !RegisterSemantic(NodeSemanticId, TEXT("deformer_graph_node"), NodeIdentityModelId, EWrite::DomainMutation, OutError)
			|| !RegisterSemantic(NodePropertySemanticId, TEXT("deformer_node_property"), NodePropertyIdentityModelId, EWrite::PropertyAssignment, OutError)
			|| !RegisterSemantic(EdgeSemanticId, TEXT("deformer_graph_edge"), EdgeIdentityModelId, EWrite::DomainMutation, OutError)) return false;
		UeAgentAuthoringState::FStateAccessorDescriptor Accessor;
		Accessor.AccessorId = StateAccessorId;
		Accessor.TargetKinds = {TargetKind, TEXT("deformer_graph_node"), TEXT("deformer_graph_edge")};
		Accessor.CapabilityTags = {TEXT("graph"), TEXT("node"), TEXT("connection")};
		UeAgentAuthoringState::ERegistrationResult StateResult;
		if (!UeAgentAuthoringState::FStateAccessorRegistry::Get().RegisterAccessor(
			Accessor, ReadState, StateResult, OutError)) return false;

		UeAgentExportContract::FContract Contract;
		Contract.Schema = TEXT("uai.export_contract.v2"); Contract.ContractId = ExportContractId;
		Contract.ContractVersion = TEXT("2.0.0"); Contract.TypeId = TEXT("deformer_graph");
		Contract.SemanticDescriptorRefs = {GraphSemanticId, ExportTargetSemanticId, NodeSemanticId, EdgeSemanticId};
		Contract.DefaultProfile = TEXT("llm_authoring");
		auto Profile = [](
			const TCHAR* Id,
			TArray<FString> Roles)
		{
			UeAgentExportContract::FProfileDescriptor Result;
			Result.ProfileId = Id;
			Result.IncludedRoles = MoveTemp(Roles);
			return Result;
		};
		Contract.Profiles = {
			Profile(TEXT("llm_authoring"), {TEXT("identity"), TEXT("authoring")}),
			Profile(TEXT("readback"), {TEXT("identity"), TEXT("authoring"), TEXT("readback")}),
			Profile(TEXT("debug"), {TEXT("identity"), TEXT("authoring"), TEXT("readback"), TEXT("diagnostics"), TEXT("validation")}),
			Profile(TEXT("validation"), {TEXT("identity"), TEXT("validation")}),
		};
		UeAgentExportContract::FFileDescriptor File;
		File.FileId = TEXT("index"); File.Path = TEXT("index.hjson"); File.Role = TEXT("identity");
		File.Profiles = {TEXT("llm_authoring"), TEXT("readback"), TEXT("debug"), TEXT("validation")};
		File.bRequired = true; File.ProjectionId = ProjectionId;
		File.MergePolicy = TEXT("inline"); Contract.Files = {File};
		Contract.Fields = {
			Field(TEXT("operation"), TEXT("index.hjson#/operation"), UeAgentExportContract::EFieldSourceKind::FormatConstant, TEXT(""), TEXT("uai.export.format.operation_update"), false),
			Field(TEXT("target"), TEXT("index.hjson#/target"), UeAgentExportContract::EFieldSourceKind::Semantic, ExportTargetSemanticId, TEXT("uai.export.semantic.value"), false),
			Field(TEXT("nodes"), TEXT("index.hjson#/provider_extension/graph/nodes/*"), UeAgentExportContract::EFieldSourceKind::Semantic, NodeSemanticId, TEXT("uai.export.semantic.value"), true),
			Field(TEXT("edges"), TEXT("index.hjson#/provider_extension/graph/edges/*"), UeAgentExportContract::EFieldSourceKind::Semantic, EdgeSemanticId, TEXT("uai.export.semantic.value"), true)};
		Contract.WriterPolicy.Format = TEXT("hjson");
		Contract.WriterPolicy.InlineStructMaxFields = 6;
		Contract.WriterPolicy.bListCountsAsOneField = true;
		Contract.WriterPolicy.bScalarListInline = true;
		Contract.WriterPolicy.KnownStructComponents = TEXT("array");
		Contract.WriterPolicy.TransformShape = TEXT("named_arrays");
		Contract.WriterPolicy.PreserveValueTextProfiles = {TEXT("readback"), TEXT("debug")};
		Contract.WriterPolicy.RemoveFields = {TEXT("operations"), TEXT("control_type"), TEXT("category_path")};
		Contract.WriterPolicy.PropertyPathPolicy = TEXT("only_when_needed_for_edit");
		Contract.WriterPolicy.UnknownOutputFieldPolicy = TEXT("reject");
		Contract.ExtensionPolicy.Input = TEXT("shared_state_snapshot_only");
		Contract.ExtensionPolicy.bDirectUObjectRead = false;
		Contract.SortPolicy.bDeterministic = true;
		Contract.SortPolicy.bIdentityBeforeContent = true;
		UeAgentExportContract::ERegistrationResult ContractResult;
		if (!UeAgentExportContract::FRegistry::Get().RegisterContract(Contract, ContractResult, OutError)) return false;
		UeAgentCompoundProjection::FExtensionDescriptor Extension;
		Extension.ProjectionId = ProjectionId; Extension.ImplementationId = TEXT("deformer.optimus.capture.v1");
		Extension.SourceKinds = {UeAgentExportContract::EFieldSourceKind::Semantic};
		UeAgentCompoundProjection::ERegistrationResult ProjectionResult;
		if (!UeAgentCompoundProjection::FRegistry::Get().RegisterExtension(
			Extension, Project, ProjectionResult, OutError)) return false;
		bRegistered = true; return true;
	}

	bool ExportWorkspace(UOptimusDeformer& Asset, const FString& WorkspaceRoot,
		const FString& ProfileId, UeAgentExportWorkspaceWriter::FWorkspaceResult& OutResult)
	{
  auto& OutError = OutResult.Diagnostic;
		TArray<UeAgentAuthoringState::FReadRequest> Requests;
		if (!BuildExportSnapshotReadRequests(Asset, Requests, OutError)) return false;
		UeAgentAuthoringState::FCaptureResult Capture;
		if (!UeAgentAuthoringState::CaptureSnapshot(Requests, Capture))
		{
			OutError = Capture.Diagnostic; return false;
		}
		return UE::UAI::HjsonWorkspace::TryExportCompositionWorkspace(CompositionKind, ProfileId, Capture.Snapshot, WorkspaceRoot, OutResult);
	}
}

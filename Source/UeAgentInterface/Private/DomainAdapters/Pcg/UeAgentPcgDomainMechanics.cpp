#include "DomainAdapters/Pcg/UeAgentPcgDomainMechanics.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Core/ContentDiscovery/UeAgentReadOnlyCandidateBatch.h"

#include "Composition/HjsonImport/UeAgentSemanticWorkspaceImport.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/HjsonWorkspace/UeAgentGraphAuthoringProjection.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "UObject/ObjectMacros.h"
#include "Domains/PCG/Authoring/UeAgentPCGAuthoringApplyProvider.h"
#include "Domains/PCG/Authoring/UeAgentPCGGraphAuthoring.h"
#include "Domains/PCG/Authoring/UeAgentPCGAuthoringDescriptors.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Domains/PCG/Authoring/UeAgentPCGAuthoringExport.h"
#include "Domains/PCG/Authoring/UeAgentPCGAuthoringProjection.h"
#include "Domains/PCG/Authoring/UeAgentPCGAuthoringRegistration.h"
#include "Domains/PCG/Authoring/UeAgentPCGAuthoringState.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "OperationOwners/PcgCommands/UeAgentPcgCommandOperationOwner.h"
#include "PCGGraph.h"
#include "QueryOwners/Pcg/UeAgentPcgNodeCandidateQuery.h"
#include "UObject/Package.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::DomainAdapters::Pcg
{
	using namespace HjsonWorkspace;
	using namespace HjsonWorkspace::DomainAuthoring;

	namespace
	{
		const FString SchemaId = TEXT("uai.pcg.edit/1");
		const FString GraphType = TEXT("/Script/PCG.PCGGraph");
		const FString NodeCarrierType = TEXT("/Script/PCG.PCGNode");

		bool Fail(FWorkspaceError& OutError, UE::UAI::Contract::FDiagnostic Diagnostic)
        {
            OutError.SetFailure(MoveTemp(Diagnostic));
            return false;
        }

		UE::UAI::Contract::FDiagnostic PcgFact(const TCHAR* Code, const TCHAR* Reason,
			const FString& Path, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
			const FString& Target = FString(), const FString& SourceKind = FString())
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("authoring_apply"));
			Error.PropertyPath = Path;
			Error.SemanticPointer = Path;
			Error.TargetPath = Target;
			Error.SourceKind = SourceKind;
			Error.ActualValuePresent = Actual.IsValid();
			Error.ExpectedValuePresent = Expected.IsValid();
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			return Error;
		}

		TSharedPtr<FJsonValue> PcgJsonFact(const TSharedPtr<FJsonValue>& Value)
		{
			if (!Value.IsValid() || (Value->Type != EJson::Object && Value->Type != EJson::Array)) return Value;
			const auto Shape = MakeShared<FJsonObject>();
			Shape->SetStringField(TEXT("kind"), Value->Type == EJson::Object ? TEXT("object") : TEXT("array"));
			Shape->SetNumberField(TEXT("count"), Value->Type == EJson::Object ? Value->AsObject()->Values.Num() : Value->AsArray().Num());
			return MakeShared<FJsonValueObject>(Shape);
		}

		FString PackageIdentity(const FString& Target)
		{
			return Target.Contains(TEXT("."))
				? FPackageName::ObjectPathToPackageName(Target)
				: Target;
		}

		bool AppendResolvedNodeProperties(
			const Composition::HjsonImport::FPreparedAuthoringImport& Prepared,
			const Composition::HjsonImport::FSemanticWorkspaceChangeProjection& Projection,
			const TSharedPtr<FJsonObject>& GraphDocument, UE::UAI::Contract::FDiagnostic& OutError)
		{
			const TSharedPtr<FJsonObject>* Nodes = nullptr;
			if (!GraphDocument->TryGetObjectField(TEXT("nodes"), Nodes) || !Nodes || !Nodes->IsValid())
			{
				OutError = PcgFact(TEXT("pcg_semantic_nodes_missing"), TEXT("The projected PCG graph has no valid nodes object."),
					TEXT("/nodes"), PcgJsonFact(GraphDocument->TryGetField(TEXT("nodes"))),
					MakeShared<FJsonValueString>(TEXT("object")), FString(), TEXT("native_result"));
				return false;
			}
			for (const auto& Pair : Projection.PropertyChangesByUnitId)
			{
				const auto* Unit = Prepared.GetWorkspace()->GetModel()->FindUnit(Pair.Key);
				if (!Unit)
				{
					OutError = PcgFact(TEXT("pcg_property_unit_missing"), TEXT("The projected PCG property change refers to a workspace unit that is unavailable."),
						TEXT("/property_changes/unit_available"), MakeShared<FJsonValueBoolean>(false),
						MakeShared<FJsonValueBoolean>(true), FString(), TEXT("native_result"));
					OutError.InternalContext = Pair.Key;
					return false;
				}
				const auto* Change = Prepared.FindChangeForTargetIdentity(Unit->GetTargetIdentityKey());
				if (!Change)
				{
					OutError = PcgFact(TEXT("pcg_property_change_missing"), TEXT("No prepared PCG change matches the projected property unit."),
						TEXT("/property_changes/prepared_change_available"), MakeShared<FJsonValueBoolean>(false),
						MakeShared<FJsonValueBoolean>(true), FString(), TEXT("native_result"));
					OutError.InternalContext = Unit->GetTargetIdentityKey();
					return false;
				}
				const FString NodeId = Change->GetIdentity().GetSelector(TEXT("node_id"));
				if (NodeId.IsEmpty()) continue; // Root descriptors are consumed by the reflection wrapper.
				const TSharedPtr<FJsonObject>* Node = nullptr;
				if (!(*Nodes)->TryGetObjectField(NodeId, Node) || !Node || !Node->IsValid())
				{
					OutError = PcgFact(TEXT("pcg_property_node_missing"), TEXT("The projected PCG node needed for the property change is unavailable."),
						TEXT("/property_changes/node_available"), MakeShared<FJsonValueBoolean>(false),
						MakeShared<FJsonValueBoolean>(true), FString(), TEXT("native_result"));
					OutError.InternalContext = NodeId;
					return false;
				}
				const TArray<TSharedPtr<FJsonValue>>* Existing = nullptr;
				(*Node)->TryGetArrayField(TEXT("properties"), Existing);
				TArray<TSharedPtr<FJsonValue>> Properties = Existing ? *Existing : TArray<TSharedPtr<FJsonValue>>();
				Properties.Add(Pair.Value);
				(*Node)->SetArrayField(TEXT("properties"), MoveTemp(Properties));
			}
			return true;
		}

		bool EnsureAuthoringRegistered(FWorkspaceError& OutError)
		{
			static const TPair<bool, UE::UAI::Contract::FDiagnostic> Registration = []()
			{
				UE::UAI::Contract::FDiagnostic Error;
				const bool bRegistered =
					UeAgentPCGAuthoring::RegisterAuthoring(Error);
				return TPair<bool, UE::UAI::Contract::FDiagnostic>(bRegistered, MoveTemp(Error));
			}();
			if (!Registration.Key)
			{
				return Fail(OutError, Registration.Value.IsValid() ? Registration.Value :
					PcgFact(TEXT("pcg_domain_authoring_registration_failed"),
						TEXT("PCG authoring registration failed without a lower diagnostic."),
						TEXT("/authoring/registered"), MakeShared<FJsonValueBoolean>(false),
						MakeShared<FJsonValueBoolean>(true), FString(), TEXT("native_result")));
			}
			return true;
		}

		bool TryRead(
			const FString& Target,
			FString& OutPayload,
			FString& OutRevision,
			TSharedPtr<FJsonObject>* OutSnapshot,
			UObject** OutObject,
			FWorkspaceError& OutError)
		{
			UObject* Owner = nullptr;
			DomainAdapters::ContentGraphRead::FUeAgentContentGraphCapture Capture;
			if (!TryCaptureObject(
					Target, GraphType, Owner, Capture, OutError))
			{
				return false;
			}
			TSharedPtr<FJsonObject> Snapshot;
			if (!TryBuildCaptureSnapshot(
					Target, GraphType, Capture, Snapshot, OutError))
			{
				return false;
			}
			OutRevision = Capture.Revision;
			if (OutSnapshot) *OutSnapshot = Snapshot;
			if (OutObject) *OutObject = Owner;
			return TryCanonicalizeObject(Snapshot, OutPayload, OutError);
		}

		bool ValidateMissingTarget(
			const FString& Target,
			FWorkspaceError& OutError)
		{
			bool bPresent = false;
			if (!TryInspectObjectTargetPresence(Target, bPresent))
			{
				return Fail(OutError, PcgFact(TEXT("pcg_domain_target_invalid"),
					TEXT("The PCG target is not a valid asset object or package reference."), TEXT("/target"),
					MakeShared<FJsonValueString>(Target), MakeShared<FJsonValueString>(TEXT("valid asset object or package reference")),
					Target, TEXT("command_input")));
			}
			if (bPresent)
			{
				return Fail(OutError, PcgFact(TEXT("pcg_domain_stale_revision"),
					TEXT("The PCG target already exists, but the request expects a missing target."), TEXT("/expected_revision"),
					MakeShared<FJsonValueString>(TEXT("existing")), MakeShared<FJsonValueString>(TEXT("missing")),
					Target, TEXT("command_input")));
			}
			return true;
		}

		bool NormalizeNodeCandidates(
			UObject& TargetObject,
			const TArray<TSharedPtr<FJsonValue>>& Edits,
			FWorkspaceError& OutError)
		{
			UPCGGraph* const Graph = Cast<UPCGGraph>(&TargetObject);
			if (!Graph || Graph->GetClass() != UPCGGraph::StaticClass())
			{
				return Fail(OutError, PcgFact(TEXT("pcg_candidate_target_type_unsupported"),
					TEXT("PCG node candidates require an exact PCGGraph target class."),
					TEXT("/target/class"), MakeShared<FJsonValueString>(TargetObject.GetClass()->GetPathName()),
					MakeShared<FJsonValueString>(GraphType), TargetObject.GetPathName(), TEXT("native_result")));
			}
			ContentDiscovery::FReadOnlyCandidateBatch CandidateBatch;
			for (int32 Index = 0; Index < Edits.Num(); ++Index)
			{
				const TSharedPtr<FJsonObject> Edit =
					Edits[Index].IsValid() ? Edits[Index]->AsObject() : nullptr;
				FString Intent;
				const TSharedPtr<FJsonObject>* Value = nullptr;
				if (!Edit.IsValid()
					|| !Edit->TryGetStringField(TEXT("intent"), Intent)
					|| Intent != TEXT("create")
					|| !Edit->TryGetObjectField(TEXT("value"), Value)
					|| !Value || !Value->IsValid())
				{
					continue;
				}
				const TSharedPtr<FJsonObject>* Candidate = nullptr;
				if (!(*Value)->HasField(TEXT("candidate")))
				{
					return Fail(OutError, PcgFact(TEXT("pcg_candidate_selector_required"),
						TEXT("A PCG node create edit requires a candidate selector."),
						FString::Printf(TEXT("/root/operation/params/edits/%d/value/candidate"), Index),
						nullptr, MakeShared<FJsonValueString>(TEXT("object")), TargetObject.GetPathName()));
				}
				const FString Pointer = FString::Printf(
					TEXT("/root/operation/params/edits/%d/value/candidate"),
					Index);
				if (!(*Value)->TryGetObjectField(TEXT("candidate"), Candidate)
					|| !Candidate || !Candidate->IsValid())
				{
					return Fail(OutError, PcgFact(TEXT("domain_hjson_candidate_binding_invalid"),
						TEXT("The PCG candidate selector must be an object."), Pointer,
						PcgJsonFact((*Value)->TryGetField(TEXT("candidate"))),
						MakeShared<FJsonValueString>(TEXT("object")), TargetObject.GetPathName()));
				}
				FDomainHjsonCandidateSelector Selector;
				if (!TryReadCandidateSelector(
						*Candidate, Pointer, Selector, OutError))
				{
					return false;
				}
				QueryOwners::Pcg::FPcgNodeCandidate ResolvedCandidate;
				QueryOwners::Pcg::FPcgNodeCandidateQueryError CandidateError;
				if (!QueryOwners::Pcg::TryResolvePcgNodeCandidate(
						*Graph,
						Selector.Name,
					FString(),
						ResolvedCandidate,
						CandidateError, Selector.Select, &CandidateBatch))
				{
					if (CandidateError.TargetPath.IsEmpty()) CandidateError.TargetPath = TargetObject.GetPathName();
					OutError.SetFailure(MoveTemp(CandidateError), FString(), Pointer);
					return false;
				}
				if (!ResolvedCandidate.bSupported)
				{
					return Fail(OutError, PcgFact(TEXT("pcg_candidate_action_kind_unsupported"),
						TEXT("The selected PCG node action is not supported."), Pointer,
						MakeShared<FJsonValueString>(ResolvedCandidate.ActionKind),
						MakeShared<FJsonValueString>(TEXT("supported PCG node action")), TargetObject.GetPathName()));
				}
				FString Existing;
				if ((*Value)->TryGetStringField(TEXT("carrier_type_ref"), Existing)
					&& Existing != NodeCarrierType)
				{
					return Fail(OutError, PcgFact(TEXT("pcg_candidate_carrier_conflict"),
						TEXT("The PCG create edit declares a conflicting node carrier type."),
						FString::Printf(TEXT("/root/operation/params/edits/%d/value/carrier_type_ref"), Index),
						MakeShared<FJsonValueString>(Existing), MakeShared<FJsonValueString>(NodeCarrierType),
						TargetObject.GetPathName()));
				}
				if ((*Value)->HasField(TEXT("settings_type_ref")))
				{
					return Fail(OutError, PcgFact(TEXT("pcg_candidate_legacy_settings_type_unsupported"),
						TEXT("The legacy settings_type_ref field is unsupported; select the node through candidate."),
						FString::Printf(TEXT("/root/operation/params/edits/%d/value/settings_type_ref"), Index),
						PcgJsonFact((*Value)->TryGetField(TEXT("settings_type_ref"))), nullptr, TargetObject.GetPathName()));
				}
				if ((*Value)->TryGetStringField(TEXT("action_kind"), Existing)
					&& Existing != ResolvedCandidate.ActionKind)
				{
					return Fail(OutError, PcgFact(TEXT("pcg_candidate_action_kind_mismatch"),
						TEXT("The declared PCG action kind differs from the selected candidate."),
						FString::Printf(TEXT("/root/operation/params/edits/%d/value/action_kind"), Index),
						MakeShared<FJsonValueString>(Existing), MakeShared<FJsonValueString>(ResolvedCandidate.ActionKind),
						TargetObject.GetPathName()));
				}
				(*Candidate)->SetStringField(
					TEXT("name"), ResolvedCandidate.Name);
				(*Value)->RemoveField(TEXT("carrier_type_ref"));
				(*Value)->RemoveField(TEXT("action_kind"));
			}
			return true;
		}

		bool ParseLifecycleMutation(
			const FString& Target,
			const FString& MutationPayload,
			TSharedPtr<FJsonObject>& OutMutation,
			FWorkspaceError& OutError)
		{
			return TryReadNativeMutationPayload(Target, MutationPayload, OutMutation, OutError);
		}

		bool TryImport(
			const FString& Target,
			const FString& CanonicalParamsJson,
			FString& OutPayload,
			FWorkspaceError& OutError)
		{
			FResolvedDomainHjsonDocument Document;
			if (!TryDecodeResolvedDocument(
					Target,
					CanonicalParamsJson,
					FDomainHjsonDocumentContract(
						TEXT("pcg"), SchemaId, GraphType),
					Document,
					OutError))
			{
				return false;
			}
			FScopedDomainHjsonDiagnosticSource DiagnosticSource(Document, OutError);
			if (Document.SemanticWorkspace.IsValid())
			{
				using namespace UE::UAI::Composition::HjsonImport;

				UObject* TargetObject = nullptr;
				DomainAdapters::ContentGraphRead::FUeAgentContentGraphCapture TargetCapture;
				if (!TryCaptureObject(
						Target, GraphType, TargetObject, TargetCapture, OutError))
				{
					return false;
				}
				UPCGGraph* Graph = Cast<UPCGGraph>(TargetObject);
				if (!Graph || Graph->GetClass()->GetPathName() != GraphType)
				{
					return Fail(OutError, PcgFact(TEXT("pcg_domain_target_type_invalid"),
						TEXT("PCG authoring requires an exact PCGGraph target class."), TEXT("/target/class"),
						TargetObject ? MakeShared<FJsonValueString>(TargetObject->GetClass()->GetPathName()) : TSharedPtr<FJsonValueString>(),
						MakeShared<FJsonValueString>(GraphType), Target, TEXT("native_result")));
				}
				if (!EnsureAuthoringRegistered(OutError)) return false;

				UeAgentExportContract::FContract ExportContract;
				UE::UAI::Contract::FDiagnostic ProfileError;
				if (!UeAgentPCGAuthoring::GetExportContract(ExportContract, ProfileError))
				{
					return Fail(OutError, MoveTemp(ProfileError));
				}
				if (!ExportContract.Profiles.ContainsByPredicate(
					[](const UeAgentExportContract::FProfileDescriptor& Profile)
					{
						return Profile.ProfileId == TEXT("llm_authoring");
					}))
				{
					TArray<TSharedPtr<FJsonValue>> AvailableProfiles;
					for (const auto& Profile : ExportContract.Profiles)
						AvailableProfiles.Add(MakeShared<FJsonValueString>(Profile.ProfileId));
					return Fail(OutError, PcgFact(TEXT("pcg_domain_authoring_profile_missing"),
						TEXT("The PCG export contract does not provide the required authoring profile."),
						TEXT("/authoring/profile"), MakeShared<FJsonValueArray>(MoveTemp(AvailableProfiles)),
						MakeShared<FJsonValueString>(TEXT("llm_authoring")), Target, TEXT("native_result")));
				}

				TArray<UeAgentAuthoringState::FReadRequest> Requests;
				UE::UAI::Contract::FDiagnostic CaptureError;
				if (!UeAgentPCGAuthoring::BuildAssetExportSnapshotReadRequests(
						*Graph, Requests, CaptureError))
				{
					OutError.SetFailure(CaptureError, Target);
					return false;
				}
				UeAgentAuthoringState::FCaptureResult Capture;
				if (!UeAgentAuthoringState::CaptureSnapshot(Requests, Capture))
				{
					OutError.SetFailure(Capture.Diagnostic, Target);
					return false;
				}

				ContextResource::FUeAgentResourceTracker Resources =
					MakeSemanticWorkspaceResourceTracker();
				StateSemantic::FUeAgentCanonicalSnapshotPtr CanonicalSnapshot;
				FAuthoringModelPtr Model;
				if (!TryBuildCanonicalAuthoringCapture(
					TEXT("ue.pcg_graph.asset"), TEXT("llm_authoring"),
					Capture.Snapshot, Resources, CanonicalSnapshot, Model, OutError))
				{
					return false;
				}

				Capture.Snapshot.ReleaseNativeValues();
				const FString TargetPackage = PackageIdentity(Target);
				const FString TargetObjectPath = ObjectPathFromPackageIdentity(Target);
				return TryCoordinateSemanticWorkspaceImport(
					Document.SemanticWorkspace.ToSharedRef(), Model.ToSharedRef(),
					CanonicalSnapshot.ToSharedRef(),
					Contract::FUeAgentDescriptorIdentity(
						TEXT("pcg.semantic_workspace"), 1,
						Model->GetSemanticFingerprint()),
					TargetObjectPath, Resources,
					{
						TEXT("pcg-domain-provider"), Target, TargetObjectPath,
						TEXT("pcg-semantic"),
						TEXT("pcg_semantic_prepare_failed"),
						TEXT("pcg_semantic_apply_failed"),
						TEXT("pcg_semantic_readback_payload_invalid"),
						TEXT("pcg_domain_generate_not_settled"),
						[TargetObjectPath](TArray<UeAgentAuthoringState::FReadRequest>& Requests, UE::UAI::Contract::FDiagnostic& Error)
						{
							UPCGGraph* Native = Cast<UPCGGraph>(StaticLoadObject(UPCGGraph::StaticClass(), nullptr, *TargetObjectPath));
							if (!Native)
							{
								Error = PcgFact(TEXT("pcg_semantic_readback_target_unavailable"),
									TEXT("The PCG graph needed for semantic readback could not be loaded."),
									TEXT("/target/available"), MakeShared<FJsonValueBoolean>(false),
									MakeShared<FJsonValueBoolean>(true), TargetObjectPath, TEXT("native_result"));
								return false;
							}
							return UeAgentPCGAuthoring::BuildAssetExportSnapshotReadRequests(*Native, Requests, Error);
						}},
					[TargetPackage, &Graph, &TargetObject](
						const FString& RequestId,
						const Composition::HjsonImport::FPreparedAuthoringImport& Prepared,
						Composition::HjsonImport::FSemanticWorkspaceChangeProjection&& Projection,
						FRootPropertyApplyOutcome& OutOutcome)
					{
						TSharedPtr<FJsonObject> GraphDocument;
						auto Connections = MakeShared<UE::UAI::HjsonWorkspace::FGraphAuthoringConnectionContext>();
						UE::UAI::Contract::FDiagnostic ProjectionError;
						if (!UeAgentPCGAuthoring::TryBuildNativeGraphDocumentFromSemanticModel(
							Prepared.GetWorkspace()->GetModel(), GraphDocument, ProjectionError, &Prepared.GetWorkspace().Get(), &Connections.Get())
							|| !AppendResolvedNodeProperties(Prepared, Projection, GraphDocument, ProjectionError))
						{
							if (ProjectionError.TargetPath.IsEmpty()) ProjectionError.TargetPath = TargetPackage;
							OutOutcome.Diagnostic = ProjectionError;
							OutOutcome.Phase = TEXT("semantic_projection");
							return false;
						}
						UeAgentPCGAuthoring::FAssetApplyRequest ApplyRequest;
						ApplyRequest.Operation = UeAgentApplyInputContract::EOperation::Update;
						ApplyRequest.AssetPath = TargetPackage;
						ApplyRequest.Properties = MoveTemp(Projection.RootObjectProperties);
						ApplyRequest.Graph = MoveTemp(GraphDocument);
						ApplyRequest.bPreserveExistingNodes = true;
						ApplyRequest.Connections = Connections;
						ApplyRequest.bCompile = true;
						ApplyRequest.bSave = true;
						ApplyRequest.FinalReadback = OutOutcome.FinalReadback;
						FString ParameterUnitId;
						for (const auto& Unit : Prepared.GetWorkspace()->GetModel()->GetUnits())
						{
							if (Unit.GetSemanticTypeRef() != UeAgentPCGAuthoring::ExportParametersSemanticId) continue;
							ParameterUnitId = Unit.GetUnitId();
							break;
						}
						// Parameters are a strict canonical value. Keep provenance outside
						// its schema/fields payload; the provider captures it before deferral.
						UeAgentAuthoringDiagnostics::FScopedSourceLocations ParameterSources(ApplyRequest.Graph.Get(),
							[&Prepared, ParameterUnitId](const FString& Pointer) -> UE::UAI::Contract::FSourceLocation
							{
								const FString Root = TEXT("/parameters");
								if (ParameterUnitId.IsEmpty() || (Pointer != Root && !Pointer.StartsWith(Root + TEXT("/")))) return {};
								const FString Relative = Pointer == Root ? TEXT("/") : Pointer.Mid(Root.Len());
								FString Lookup = Relative;
								const FSourceProvenance* Source = nullptr;
								for (;;)
								{
									Source = Prepared.GetWorkspace()->FindValueSource(ParameterUnitId, Lookup);
									if (Source || Lookup == TEXT("/")) break;
									int32 Slash;
									if (!Lookup.FindLastChar(TEXT('/'), Slash)) break;
									Lookup = Slash == 0 ? TEXT("/") : Lookup.Left(Slash);
								}
								if (!Source) Source = Prepared.GetWorkspace()->GetUnitSources().Find(ParameterUnitId);
								if (!Source) return {};
								UE::UAI::Contract::FSourceLocation Result;
								Result.SourceFile = Source->GetSourceUnit();
								Result.JsonPath = Source->GetJsonPointer();
								Result.Line = Source->GetLine();
								Result.Column = Source->GetColumn();
								Result.SourceUnit = ParameterUnitId;
								Result.InternalJsonPath = Relative;
								return Result;
							});
						UeAgentPCGAuthoring::FAssetApplyResult ApplyResult;
						const bool bApplied = UeAgentPCGAuthoring::ApplyAsset(
							ApplyRequest, ApplyResult, RequestId);
						OutOutcome.bApplied = bApplied && ApplyResult.IsApplied();
						OutOutcome.Phase = MoveTemp(ApplyResult.Phase);
						OutOutcome.Diagnostic = MoveTemp(ApplyResult.Diagnostic);
						ApplyResult.Asset = nullptr;
						Graph = nullptr;
						TargetObject = nullptr;
						return bApplied;
					},
					[&Target](FString& OutReadbackPayload, FString& OutRevision,
						FWorkspaceError& OutReadError)
					{
						return TryRead(
							Target, OutReadbackPayload, OutRevision, nullptr, nullptr,
							OutReadError);
					},
					OutPayload, OutError);
			}

			FString MutationPayload;
			if (Document.ExpectedRevision == TEXT("missing"))
			{
				if (!ValidateMissingTarget(Target, OutError)) return false;
				const TSharedPtr<FJsonObject>* Create = nullptr;
				if (!Document.Root->TryGetObjectField(TEXT("create"), Create)
					|| !Create || !Create->IsValid())
				{
					return Fail(OutError, PcgFact(TEXT("pcg_domain_create_contract_required"),
						TEXT("Creating a missing PCG graph requires a create object."), TEXT("/root/create"),
						PcgJsonFact(Document.Root->TryGetField(TEXT("create"))),
						MakeShared<FJsonValueString>(TEXT("object")), Target));
				}
				if (Document.Root->HasField(TEXT("operation")))
				{
					return Fail(OutError, PcgFact(TEXT("pcg_domain_create_then_edit_not_atomic"),
						TEXT("A PCG create document cannot also contain an operation; create and edit are separate imports."),
						TEXT("/root/operation"), PcgJsonFact(Document.Root->TryGetField(TEXT("operation"))), nullptr, Target));
				}
				const FString Package = PackageIdentity(Target);
				const FString ExpectedName =
					FPackageName::GetLongPackageAssetName(Package);
				const FString ExpectedPath =
					FPackageName::GetLongPackagePath(Package);
				FString Declared;
				if ((*Create)->TryGetStringField(TEXT("asset_name"), Declared)
					&& Declared != ExpectedName)
				{
					return Fail(OutError, PcgFact(TEXT("pcg_domain_create_name_mismatch"),
						TEXT("The declared PCG asset name does not match the import target."), TEXT("/root/create/asset_name"),
						MakeShared<FJsonValueString>(Declared), MakeShared<FJsonValueString>(ExpectedName), Target));
				}
				if ((*Create)->TryGetStringField(TEXT("package_path"), Declared))
				{
					while (Declared.EndsWith(TEXT("/")))
					{
						Declared.LeftChopInline(1);
					}
					if (Declared != ExpectedPath)
					{
						return Fail(OutError, PcgFact(TEXT("pcg_domain_create_path_mismatch"),
							TEXT("The declared PCG package path does not match the import target."), TEXT("/root/create/package_path"),
							MakeShared<FJsonValueString>(Declared), MakeShared<FJsonValueString>(ExpectedPath), Target));
					}
				}
				const TSharedPtr<FJsonObject> Native =
					MakeShared<FJsonObject>(**Create);
				Native->SetStringField(TEXT("asset_name"), ExpectedName);
				Native->SetStringField(TEXT("package_path"), ExpectedPath);
				UE::UAI::Contract::FDiagnostic MechanicsError;
				if (!OperationOwners::PcgCommands::TryCreatePcgGraphAsset(
						Native.ToSharedRef(), FString(), MutationPayload,
						MechanicsError))
				{
					Document.LocateDiagnostic(MechanicsError, TEXT("/root/create"));
					SetWorkspaceErrorFromCommandFailure(MechanicsError, TEXT("pcg_domain_native_command_failed"), Target, OutError);
					return false;
				}
			}
			else
			{
				FString BeforePayload;
				FString CurrentRevision;
				UObject* TargetObject = nullptr;
				if (!TryRead(
						Target, BeforePayload, CurrentRevision, nullptr,
						&TargetObject, OutError))
				{
					return false;
				}
				if (Document.ExpectedRevision != CurrentRevision)
				{
					auto Error = PcgFact(TEXT("pcg_domain_stale_revision"),
						TEXT("The requested PCG revision no longer matches the current graph state."), TEXT("/expected_revision"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true),
						Target, TEXT("command_input"));
					Error.ActualEvidence = MakeShared<FJsonValueString>(CurrentRevision);
					Error.ExpectedEvidence = MakeShared<FJsonValueString>(Document.ExpectedRevision);
					return Fail(OutError, MoveTemp(Error));
				}
				if (Document.Root->HasField(TEXT("create")))
				{
					return Fail(OutError, PcgFact(TEXT("pcg_domain_create_requires_missing"),
						TEXT("A PCG create object requires a missing target, but the target already exists."),
						TEXT("/root/create"), MakeShared<FJsonValueString>(TEXT("existing")),
						MakeShared<FJsonValueString>(TEXT("missing")), Target));
				}
				const TSharedPtr<FJsonObject>* Operation = nullptr;
				const TSharedPtr<FJsonObject>* Params = nullptr;
				FString CommandId;
				if (!Document.Root->TryGetObjectField(TEXT("operation"), Operation) || !Operation || !Operation->IsValid())
				{
					return Fail(OutError, PcgFact(TEXT("pcg_domain_operation_contract_required"),
						TEXT("Updating a PCG graph requires an operation object."), TEXT("/root/operation"),
						PcgJsonFact(Document.Root->TryGetField(TEXT("operation"))),
						MakeShared<FJsonValueString>(TEXT("object")), Target));
				}
				if (!(*Operation)->TryGetStringField(TEXT("command_id"), CommandId))
				{
					return Fail(OutError, PcgFact(TEXT("pcg_domain_operation_contract_required"),
						TEXT("The PCG operation requires a string command_id."), TEXT("/root/operation/command_id"),
						PcgJsonFact((*Operation)->TryGetField(TEXT("command_id"))),
						MakeShared<FJsonValueString>(TEXT("string")), Target));
				}
				if (!(*Operation)->TryGetObjectField(TEXT("params"), Params) || !Params || !Params->IsValid())
				{
					return Fail(OutError, PcgFact(TEXT("pcg_domain_operation_contract_required"),
						TEXT("The PCG operation requires a params object."), TEXT("/root/operation/params"),
						PcgJsonFact((*Operation)->TryGetField(TEXT("params"))),
						MakeShared<FJsonValueString>(TEXT("object")), Target));
				}
				if (CommandId != TEXT("pcg.graph.apply")
					&& CommandId != TEXT("pcg.graph_parameters.apply"))
				{
					return Fail(OutError, PcgFact(TEXT("pcg_domain_operation_not_supported"),
						TEXT("The PCG workspace operation command is unsupported."), TEXT("/root/operation/command_id"),
						MakeShared<FJsonValueString>(CommandId),
						MakeShared<FJsonValueString>(TEXT("pcg.graph.apply | pcg.graph_parameters.apply")), Target));
				}
				const TSharedPtr<FJsonObject> Native =
					MakeShared<FJsonObject>(**Params);
				FString NativeTarget;
				if (Native->TryGetStringField(TEXT("target_ref"), NativeTarget)
					&& ObjectPathFromPackageIdentity(NativeTarget)
						!= ObjectPathFromPackageIdentity(Target))
				{
					return Fail(OutError, PcgFact(TEXT("pcg_domain_operation_target_mismatch"),
						TEXT("The PCG operation target differs from the workspace import target."),
						TEXT("/root/operation/params/target_ref"), MakeShared<FJsonValueString>(NativeTarget),
						MakeShared<FJsonValueString>(Target), Target));
				}
				Native->SetStringField(
					TEXT("target_ref"), ObjectPathFromPackageIdentity(Target));
				if (CommandId == TEXT("pcg.graph.apply"))
				{
					const TArray<TSharedPtr<FJsonValue>>* Edits = nullptr;
					if (!Native->TryGetArrayField(TEXT("edits"), Edits)
						|| !Edits)
					{
						return Fail(OutError, PcgFact(TEXT("pcg_domain_edits_required"),
							TEXT("A PCG graph apply operation requires an edits array."), TEXT("/root/operation/params/edits"),
							PcgJsonFact(Native->TryGetField(TEXT("edits"))),
							MakeShared<FJsonValueString>(TEXT("array")), Target));
					}
					if (!NormalizeNodeCandidates(*TargetObject, *Edits, OutError))
					{
						return false;
					}
				}
				UE::UAI::Contract::FDiagnostic MechanicsError;
				const bool bApplied = CommandId == TEXT("pcg.graph.apply")
					? OperationOwners::PcgCommands::TryApplyPcgGraph(
						Native.ToSharedRef(), FString(), MutationPayload,
						MechanicsError)
					: OperationOwners::PcgCommands::TryApplyPcgGraphParameters(
						Native.ToSharedRef(), FString(), MutationPayload,
						MechanicsError);
				if (!bApplied)
				{
					Document.LocateDiagnostic(MechanicsError, TEXT("/root/operation/params"));
					SetWorkspaceErrorFromCommandFailure(MechanicsError, TEXT("pcg_domain_native_command_failed"), Target, OutError);
					return false;
				}
			}

			TSharedPtr<FJsonObject> Mutation;
			FDomainHjsonImportLifecycleReceipt LifecycleReceipt;
			if (!ParseLifecycleMutation(
					Target, MutationPayload, Mutation, OutError))
			{
				return false;
			}
			FString AfterPayload;
			FString AfterRevision;
			TSharedPtr<FJsonObject> AfterSnapshot;
			if (!TryRead(
					Target, AfterPayload, AfterRevision, &AfterSnapshot,
					nullptr, OutError))
			{
				return false;
			}
			if (!TryValidateReadbackDiagnostics(AfterSnapshot, Target,
				TEXT("pcg_domain_generate_not_settled"), OutError))
				return false;
			LifecycleReceipt.bMutationApplied = true;
			LifecycleReceipt.bNativeUpdateSettled = true;
			LifecycleReceipt.bPackageSaved = true;
			return TryBuildImportSuccessPayload(
				TEXT("pcg-domain-provider"), Target, AfterRevision,
				LifecycleReceipt, AfterSnapshot, Mutation,
				OutPayload, OutError);
		}

		bool TryExport(
			const FString& Target,
			const TSharedPtr<FJsonObject>& Params,
			FString& OutPayload,
			FWorkspaceError& OutError)
		{
			FString WorkspacePath;
			if (!Params->TryGetStringField(TEXT("workspace_path"), WorkspacePath)
				|| WorkspacePath.IsEmpty())
			{
				return Fail(OutError, PcgFact(TEXT("pcg_domain_export_path_required"),
					TEXT("Exporting a PCG workspace requires a non-empty workspace_path."), TEXT("/workspace_path"),
					PcgJsonFact(Params->TryGetField(TEXT("workspace_path"))),
					MakeShared<FJsonValueString>(TEXT("non-empty string")), Target, TEXT("command_input")));
			}
			UObject* TargetObject = nullptr;
			DomainAdapters::ContentGraphRead::FUeAgentContentGraphCapture Capture;
			if (!TryCaptureObject(
					Target, GraphType, TargetObject, Capture, OutError))
			{
				return false;
			}
			UPCGGraph* Graph = Cast<UPCGGraph>(TargetObject);
			if (!Graph || Graph->GetClass()->GetPathName() != GraphType)
			{
				return Fail(OutError, PcgFact(TEXT("pcg_domain_target_type_invalid"),
					TEXT("PCG workspace export requires an exact PCGGraph target class."), TEXT("/target/class"),
					TargetObject ? MakeShared<FJsonValueString>(TargetObject->GetClass()->GetPathName()) : TSharedPtr<FJsonValueString>(),
					MakeShared<FJsonValueString>(GraphType), Target, TEXT("native_result")));
			}
			if (!EnsureAuthoringRegistered(OutError)) return false;
			UeAgentExportWorkspaceWriter::FWorkspaceResult ExportResult;
			if (!UeAgentPCGAuthoring::ExportAssetWorkspace(*Graph, WorkspacePath, TEXT("llm_authoring"), ExportResult))
			{
				OutError.SetFailure(ExportResult.Diagnostic, Target);
				return false;
			}
			const TSharedPtr<FJsonObject> Result = ExportResult.ToPublicJsonObject();
			return TryCanonicalizeObject(Result, OutPayload, OutError);
		}
	}

	bool TryExecuteContentGraph(
		const FString& CommandId,
		const FString& TargetIdentity,
		const FString& RootTypeIdentity,
		const FString& CanonicalParamsJson,
		FString& OutPayloadJson,
		bool& bOutReadOnly,
		FWorkspaceError& OutError)
	{
		OutPayloadJson.Reset();
		bOutReadOnly = false;
		OutError.Reset();
		TSharedPtr<FJsonObject> Params;
		UE::UAI::Contract::FDiagnostic JsonError;
		if (!CommandContract::Json::TryParseObject(CanonicalParamsJson, Params, JsonError))
		{
			return Fail(OutError, MoveTemp(JsonError));
		}
		FString Target;
		if (!Params->TryGetStringField(TEXT("target"), Target)
			|| ObjectPathFromPackageIdentity(Target)
				!= ObjectPathFromPackageIdentity(TargetIdentity))
		{
			return Fail(OutError, PcgFact(TEXT("pcg_domain_target_identity_mismatch"),
				TEXT("The PCG command target is missing or differs from the bound target."), TEXT("/target"),
				PcgJsonFact(Params->TryGetField(TEXT("target"))), MakeShared<FJsonValueString>(TargetIdentity),
				TargetIdentity, TEXT("command_input")));
		}
		if (RootTypeIdentity != GraphType)
		{
			return Fail(OutError, PcgFact(TEXT("pcg_domain_root_type_invalid"),
				TEXT("The PCG domain received an unsupported root type."), TEXT("/root_type"),
				MakeShared<FJsonValueString>(RootTypeIdentity), MakeShared<FJsonValueString>(GraphType),
				Target, TEXT("command_input")));
		}
		if (CommandId == TEXT("content_graph.import_workspace"))
		{
			return TryImport(
				Target, CanonicalParamsJson, OutPayloadJson, OutError);
		}
		if (CommandId == TEXT("content_graph.export_workspace"))
		{
			bOutReadOnly = true;
			return TryExport(Target, Params, OutPayloadJson, OutError);
		}
		if (CommandId.StartsWith(TEXT("content_graph.")))
		{
			FString Revision;
			bOutReadOnly = true;
			return TryRead(
				Target, OutPayloadJson, Revision, nullptr, nullptr, OutError);
		}
		return Fail(OutError, PcgFact(TEXT("pcg_domain_command_not_supported"),
			TEXT("The command is unsupported by the PCG content graph domain."), TEXT("/command_id"),
			MakeShared<FJsonValueString>(CommandId), MakeShared<FJsonValueString>(TEXT("content_graph.*")),
			Target, TEXT("command_input")));
	}
}

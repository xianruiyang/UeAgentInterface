#include "DomainAdapters/Sequence/UeAgentSequenceDomainMechanics.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Composition/HjsonImport/UeAgentSemanticWorkspaceImport.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"
#include "OperationOwners/TimelineSequenceCommands/UeAgentTimelineSequenceCommandOperationOwner.h"
#include "UObject/ObjectMacros.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringApplyProvider.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringAssets.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringDescriptors.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringExport.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringRegistration.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringState.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END

namespace UE::UAI::DomainAdapters::Sequence
{
	using namespace CommandEdge;
	using namespace CommandContract;
	using namespace ContextResource;
	using namespace HjsonWorkspace;
	using namespace HjsonWorkspace::DomainAuthoring;
	using namespace DomainAdapters::ContentGraphRead;

	namespace
	{
		const FString SchemaId = TEXT("uai.sequence.edit/1");
		const FString RootType = TEXT("/Script/LevelSequence.LevelSequence");

		bool Fail(FWorkspaceError& OutError, UE::UAI::Contract::FDiagnostic Diagnostic,
            const FString& Target = FString(), const FString& Pointer = FString())
        {
            OutError.SetFailure(MoveTemp(Diagnostic), Target, Pointer);
            return false;
        }

		Contract::FDiagnostic ValueFailure(const TCHAR* Code, const FString& Reason, const FString& Path,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected, const FString& Target,
			const FString& SourceKind = TEXT("native_result"))
		{
			auto Diagnostic = Contract::FDiagnostic::Failure(Code, Reason, TEXT("authoring_apply"));
			Diagnostic.TargetPath = Target;
			Diagnostic.PropertyPath = Path;
			Diagnostic.SourceKind = SourceKind;
			Diagnostic.ActualValuePresent = Actual.IsValid();
			Diagnostic.ExpectedValuePresent = true;
			Diagnostic.ActualValue = MoveTemp(Actual);
			Diagnostic.ExpectedValue = MoveTemp(Expected);
			if (SourceKind != TEXT("native_result")) Diagnostic.Source.JsonPath = Path;
			return Diagnostic;
		}

		const TCHAR* JsonKind(const TSharedPtr<FJsonValue>& Value)
		{
			if (!Value.IsValid()) return TEXT("missing");
			switch (Value->Type)
			{
			case EJson::Null: return TEXT("null");
			case EJson::String: return TEXT("string");
			case EJson::Number: return TEXT("number");
			case EJson::Boolean: return TEXT("boolean");
			case EJson::Array: return TEXT("array");
			case EJson::Object: return TEXT("object");
			default: return TEXT("invalid");
			}
		}

		Contract::FDiagnostic NativeShapeFailure(const FString& Path, const TCHAR* Expected,
			const TSharedPtr<FJsonValue>& Actual, const FString& Target)
		{
			return ValueFailure(TEXT("sequence_domain_observed_state_missing"),
				TEXT("The native sequence readback does not have the required field shape."), Path,
				Actual.IsValid() ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(JsonKind(Actual))) : nullptr,
				MakeShared<FJsonValueString>(Expected), Target);
		}

		FString Sha256(const FString& Text)
		{
			FTCHARToUTF8 Utf8(*Text);
			uint8 Digest[SHA256_DIGEST_LENGTH];
			SHA256(
				reinterpret_cast<const unsigned char*>(Utf8.Get()),
				static_cast<size_t>(Utf8.Length()),
				Digest);
			FString Result(TEXT("sha256:"));
			for (const uint8 Byte : Digest)
			{
				Result += FString::Printf(TEXT("%02x"), Byte);
			}
			return Result;
		}

		bool SubmitTimelineCommand(
			const FString& CommandId,
			const FString& Target,
			const TSharedPtr<FJsonObject>& Params,
			FString& OutPayload,
			FWorkspaceError& OutError)
		{
			OutPayload.Reset();
			FString ParamsJson;
			if (!TryCanonicalizeObject(Params, ParamsJson, OutError))
			{
				return false;
			}
			UE::UAI::Contract::FDiagnostic CommandDiagnostic;
			if (!OperationOwners::TimelineSequenceCommands::TryExecuteInternal(
					CommandId,
					ParamsJson,
					Contract::FUeAgentCallerContext(
						TEXT("sequence-domain-provider"),
						{TEXT("uai:query"), TEXT("uai:operate")},
						TEXT("sequence-domain-internal")),
					OutPayload,
					CommandDiagnostic))
			{
				OutError.SetFailure(CommandDiagnostic, Target);
				return false;
			}
			return true;
		}

		TSharedPtr<FJsonObject> ReadParams(const FString& Target)
		{
			const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
			Input->SetBoolField(TEXT("include_bindings"), true);
			Input->SetBoolField(TEXT("include_tracks"), true);
			Input->SetBoolField(TEXT("include_keys"), true);
			const TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
			Params->SetStringField(TEXT("target"), Target);
			Params->SetObjectField(TEXT("input"), Input);
			return Params;
		}

		bool ParsePayload(
			const FString& PayloadJson,
			const FString& Target,
			TSharedPtr<FJsonObject>& OutPayload,
			FWorkspaceError& OutError)
		{
			UE::UAI::Contract::FDiagnostic Error;
			if (!Json::TryParseObject(PayloadJson, OutPayload, Error)
				|| !OutPayload.IsValid())
			{
				if (Error.IsEmpty()) Error = ValueFailure(TEXT("sequence_domain_native_payload_invalid"),
					TEXT("The timeline read command did not return a decodable object payload."), TEXT("payload_object_available"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Target);
				Error.SourceKind = TEXT("native_result");
				return Fail(OutError, MoveTemp(Error), Target);
			}
			return true;
		}

		bool StableRevision(
			const TSharedPtr<FJsonObject>& ReadPayload, const FString& Target,
			FString& OutRevision, FWorkspaceError& OutError)
		{
			OutRevision.Reset();
			if (!ReadPayload.IsValid()) return Fail(OutError, NativeShapeFailure(FString(), TEXT("object"), nullptr, Target));
			auto RequireObject = [&OutError, &Target](const TSharedPtr<FJsonObject>& Parent, const TCHAR* Field,
				const FString& Path, TSharedPtr<FJsonObject>& OutObject)
			{
				const auto Actual = Parent->TryGetField(Field);
				if (!Actual.IsValid() || Actual->Type != EJson::Object || !Actual->AsObject().IsValid())
					return Fail(OutError, NativeShapeFailure(Path, TEXT("object"), Actual, Target));
				OutObject = Actual->AsObject();
				return true;
			};
			TSharedPtr<FJsonObject> Node = ReadPayload;
			FString Path;
			const TCHAR* const ProjectionFields[] = {TEXT("projection"), TEXT("payload"), TEXT("snapshot")};
			for (const TCHAR* Field : ProjectionFields)
			{
				Path += TEXT("/") + FString(Field);
				if (!RequireObject(Node, Field, Path, Node)) return false;
			}
			Path += TEXT("/entries");
			const auto EntriesValue = Node->TryGetField(TEXT("entries"));
			const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
			if (!Node->TryGetArrayField(TEXT("entries"), Entries) || !Entries)
				return Fail(OutError, NativeShapeFailure(Path, TEXT("non-empty array"), EntriesValue, Target));
			if (Entries->IsEmpty()) return Fail(OutError, ValueFailure(TEXT("sequence_domain_observed_state_missing"),
				TEXT("The native sequence snapshot contains no state entries."), Path,
				MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("at least one entry")), Target));
			const auto& Entry = (*Entries)[0];
			Path += TEXT("/0");
			if (!Entry.IsValid() || Entry->Type != EJson::Object || !Entry->AsObject().IsValid())
				return Fail(OutError, NativeShapeFailure(Path, TEXT("object"), Entry, Target));
			Node = Entry->AsObject();
			const TCHAR* const StateFields[] = {TEXT("value"), TEXT("fields"), TEXT("observed_state")};
			for (const TCHAR* Field : StateFields)
			{
				Path += TEXT("/") + FString(Field);
				if (!RequireObject(Node, Field, Path, Node)) return false;
			}
			Path += TEXT("/text");
			FString State;
			if (!Node->TryGetStringField(TEXT("text"), State))
				return Fail(OutError, NativeShapeFailure(Path, TEXT("non-empty string"), Node->TryGetField(TEXT("text")), Target));
			if (State.IsEmpty()) return Fail(OutError, ValueFailure(TEXT("sequence_domain_observed_state_missing"),
				TEXT("The native sequence snapshot has empty observed state text."), Path,
				MakeShared<FJsonValueString>(State), MakeShared<FJsonValueString>(TEXT("non-empty string")), Target));
			OutRevision = Sha256(State);
			return true;
		}

		FString PackageRefFromObjectPath(const FString& Target)
		{
			int32 Dot = INDEX_NONE;
			return Target.FindLastChar(TEXT('.'), Dot)
				? Target.Left(Dot)
				: Target;
		}

		bool TryGetSequenceReflectionProfile(const FString& Target,
			const UeAgentReflectionAuthoring::FProfile*& OutProfile, Contract::FDiagnostic& OutError)
		{
			OutProfile = UeAgentReflectionAuthoring::FindProfileByTypeId(TEXT("sequence"));
			if (!OutProfile || OutProfile->AssetClassPath != RootType)
			{
				OutError = ValueFailure(TEXT("sequence_domain_reflection_profile_missing"),
					OutProfile ? TEXT("The sequence reflection profile declares the wrong asset class.")
						: TEXT("The sequence reflection profile is not registered."), TEXT("reflection_profile_asset_class"),
					OutProfile ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(OutProfile->AssetClassPath)) : nullptr,
					MakeShared<FJsonValueString>(RootType), Target);
				OutProfile = nullptr;
				return false;
			}
			return true;
		}

		bool EnsureReflectionAuthoringRegistered(const FString& Target, FWorkspaceError& OutError)
		{
			static const TPair<bool, UE::UAI::Contract::FDiagnostic> Registration = []()
			{
				UE::UAI::Contract::FDiagnostic Error;
				const bool bRegistered =
					UeAgentReflectionAuthoring::RegisterAuthoring(Error);
				return TPair<bool, UE::UAI::Contract::FDiagnostic>(bRegistered, MoveTemp(Error));
			}();
			if (!Registration.Key)
			{
				return Fail(OutError, !Registration.Value.IsEmpty() ? Registration.Value : ValueFailure(TEXT("sequence_domain_reflection_registration_failed"),
					TEXT("Sequence reflection authoring registration failed without providing a diagnostic."), TEXT("reflection_authoring_registered"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Target), Target);
			}
			return true;
		}

		bool TryRead(
			const FString& Target,
			FString& OutPayload,
			TSharedPtr<FJsonObject>& OutPayloadObject,
			FWorkspaceError& OutError)
		{
			return SubmitTimelineCommand(
					TEXT("timeline.sequence.read"),
					Target,
					ReadParams(Target),
					OutPayload,
					OutError)
				&& ParsePayload(OutPayload, Target, OutPayloadObject, OutError);
		}

		bool TryExport(
			const FString& Target,
			const TSharedPtr<FJsonObject>& Params,
			FString& OutPayloadJson,
			FWorkspaceError& OutError,
			FPreparedWorkspaceExport* PreparedExport)
		{
			FString WorkspacePath;
			FString ProfileId(TEXT("llm_authoring"));
			if (!Params.IsValid()
				|| !Params->TryGetStringField(
					TEXT("workspace_path"), WorkspacePath))
			{
				return Fail(OutError, ValueFailure(TEXT("content_graph_export_params_invalid"),
					TEXT("Sequence workspace export requires a string workspace_path."), TEXT("/workspace_path"),
					Params.IsValid() ? Params->TryGetField(TEXT("workspace_path")) : nullptr,
					MakeShared<FJsonValueString>(TEXT("absolute directory path")), Target, TEXT("command_input")), Target);
			}
			Params->TryGetStringField(TEXT("profile_id"), ProfileId);
			WorkspacePath.TrimStartAndEndInline();
			ProfileId.TrimStartAndEndInline();
			if (WorkspacePath.IsEmpty() || FPaths::IsRelative(WorkspacePath))
			{
				return Fail(OutError, ValueFailure(TEXT("workspace_path_invalid"),
					TEXT("Sequence workspace export requires a nonempty absolute directory path."), TEXT("/workspace_path"),
					MakeShared<FJsonValueString>(WorkspacePath), MakeShared<FJsonValueString>(TEXT("nonempty absolute directory path")),
					Target, TEXT("command_input")), Target);
			}
			if (ProfileId.IsEmpty())
			{
				return Fail(OutError, ValueFailure(TEXT("workspace_path_invalid"),
					TEXT("Sequence workspace export requires a nonempty profile_id."), TEXT("/profile_id"),
					MakeShared<FJsonValueString>(ProfileId), MakeShared<FJsonValueString>(TEXT("nonempty profile identifier")),
					Target, TEXT("command_input")), Target);
			}

			const UeAgentReflectionAuthoring::FProfile* Profile = nullptr;
			Contract::FDiagnostic ProfileError;
			if (!TryGetSequenceReflectionProfile(Target, Profile, ProfileError))
				return Fail(OutError, MoveTemp(ProfileError), Target);
			if (!EnsureReflectionAuthoringRegistered(Target, OutError))
			{
				return false;
			}
			const FString TargetPackage =
				UeAgentReflectionAuthoring::NormalizePackagePath(Target);
			Contract::FDiagnostic LoadError;
			UObject* Asset =
				UeAgentReflectionAuthoring::LoadAsset(*Profile, TargetPackage, LoadError);
			if (LoadError.IsValid()) return Fail(OutError, MoveTemp(LoadError), Target);
			if (!Asset || Asset->GetClass()->GetPathName() != RootType)
			{
				return Fail(OutError, ValueFailure(TEXT("sequence_domain_target_type_invalid"),
					Asset ? TEXT("The loaded sequence target has the wrong native class.") : TEXT("The sequence target could not be loaded."),
					TEXT("native_asset_class"), Asset ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Asset->GetClass()->GetPathName())) : nullptr,
					MakeShared<FJsonValueString>(RootType), Target), Target);
			}
			UeAgentExportWorkspaceWriter::FWorkspaceResult ExportResult;
			if (!UeAgentReflectionAuthoring::ExportAssetWorkspace(*Profile, *Asset, WorkspacePath, ProfileId, ExportResult, PreparedExport))
			{
				OutError.SetFailure(ExportResult.Diagnostic, Target);
				return false;
			}
			const TSharedPtr<FJsonObject> Result = ExportResult.ToPublicJsonObject();
			return TryCanonicalizeObject(Result, OutPayloadJson, OutError);
		}

		bool TryImportSemanticWorkspace(
			const FString& Target,
			const FResolvedDomainHjsonDocument& Document,
			FString& OutPayloadJson,
			FWorkspaceError& OutError)
		{
			using namespace UE::UAI::Composition::HjsonImport;

			if (!Document.SemanticWorkspace.IsValid())
			{
				return Fail(OutError, ValueFailure(TEXT("sequence_domain_semantic_workspace_missing"),
					TEXT("The resolved sequence document has no semantic workspace."), TEXT("semantic_workspace_available"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Target), Target);
			}
			const UeAgentReflectionAuthoring::FProfile* Profile = nullptr;
			Contract::FDiagnostic ProfileError;
			if (!TryGetSequenceReflectionProfile(Target, Profile, ProfileError))
				return Fail(OutError, MoveTemp(ProfileError), Target);
			if (!EnsureReflectionAuthoringRegistered(Target, OutError))
			{
				return false;
			}

			const FString TargetPackage =
				UeAgentReflectionAuthoring::NormalizePackagePath(Target);
			Contract::FDiagnostic LoadError;
			UObject* Asset =
				UeAgentReflectionAuthoring::LoadAsset(*Profile, TargetPackage, LoadError);
			if (LoadError.IsValid()) return Fail(OutError, MoveTemp(LoadError), Target);
			if (!Asset || Asset->GetClass()->GetPathName() != RootType)
			{
				return Fail(OutError, ValueFailure(TEXT("sequence_domain_target_type_invalid"),
					Asset ? TEXT("The loaded sequence target has the wrong native class.") : TEXT("The sequence target could not be loaded."),
					TEXT("native_asset_class"), Asset ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Asset->GetClass()->GetPathName())) : nullptr,
					MakeShared<FJsonValueString>(RootType), Target), Target);
			}

			TArray<UeAgentAuthoringState::FReadRequest> Requests;
			UE::UAI::Contract::FDiagnostic CaptureError;
			if (!UeAgentReflectionAuthoring::BuildAssetExportSnapshotReadRequests(
					*Profile, *Asset, Requests, CaptureError))
			{
				OutError.SetFailure(CaptureError, Target);
				return false;
			}
			SelectRootPropertyImportReadRequests(*Document.SemanticWorkspace, Requests);
			UeAgentAuthoringState::FCaptureResult Capture;
			if (!CapturePropertyImportState(*Document.SemanticWorkspace, Requests, Capture))
			{
				OutError.SetFailure(Capture.Diagnostic, Target);
				return false;
			}

			FUeAgentResourceTracker Resources =
				MakeSemanticWorkspaceResourceTracker();
			StateSemantic::FUeAgentCanonicalSnapshotPtr CanonicalSnapshot;
			FAuthoringModelPtr Model;
			const bool bCaptureBuilt = TryBuildPropertyImportCurrentCapture(Document.SemanticWorkspace.Get(),
					ResolveTargetCompositionKind(
						Profile->TargetKind, Profile->TypeId),
					TEXT("llm_authoring"),
					Capture.Snapshot,
					Resources,
					CanonicalSnapshot,
					Model,
					OutError);
			if (!bCaptureBuilt || !CanonicalSnapshot.IsValid()
				|| !Model.IsValid())
			{
				if (OutError.GetDiagnostic().IsEmpty())
				{
					const bool bSnapshotAvailable = CanonicalSnapshot.IsValid();
					return Fail(OutError, ValueFailure(TEXT("sequence_domain_semantic_capture_failed"),
						!bCaptureBuilt ? TEXT("The canonical sequence capture failed without providing a diagnostic.")
							: bSnapshotAvailable ? TEXT("The canonical sequence capture returned no authoring model and no diagnostic.")
							: TEXT("The canonical sequence capture returned no snapshot and no diagnostic."),
						!bCaptureBuilt ? TEXT("canonical_capture_succeeded")
							: bSnapshotAvailable ? TEXT("authoring_model_available") : TEXT("canonical_snapshot_available"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Target), Target);
				}
				return false;
			}

			Capture.Snapshot.ReleaseNativeValues();
			const FString TargetObjectPath =
				ObjectPathFromPackageIdentity(Target);
			return TryCoordinateSemanticWorkspaceImport(
				Document.SemanticWorkspace.ToSharedRef(), Model.ToSharedRef(),
				CanonicalSnapshot.ToSharedRef(),
				BuildSemanticWorkspaceImportDescriptor(TEXT("sequence.semantic_workspace"), *Model),
				TargetObjectPath,
				Resources,
				{
					TEXT("sequence-domain-provider"),
					Target,
					TargetObjectPath,
					TEXT("sequence-semantic"),
					TEXT("sequence_domain_semantic_prepare_failed"),
					TEXT("sequence_domain_semantic_apply_failed"),
					TEXT("sequence_domain_semantic_readback_payload_invalid"),
					TEXT("sequence_domain_compile_not_settled"),
					[Profile, TargetObjectPath](TArray<UeAgentAuthoringState::FReadRequest>& Requests, UE::UAI::Contract::FDiagnostic& Error)
					{
						UObject* Native = StaticLoadObject(UObject::StaticClass(), nullptr, *TargetObjectPath);
						if (!Native)
						{
							Error = ValueFailure(TEXT("sequence_domain_target_type_invalid"),
								TEXT("The saved sequence target could not be loaded for the export snapshot readback."),
								TEXT("native_asset_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TargetObjectPath);
							return false;
						}
						return UeAgentReflectionAuthoring::BuildAssetExportSnapshotReadRequests(*Profile, *Native, Requests, Error);
					}},
				[Profile, TargetPackage, Workspace = Document.SemanticWorkspace, &Asset](
					const FString& RequestId,
					const FPreparedAuthoringImport& Prepared,
					FSemanticWorkspaceChangeProjection&& Projection,
					FRootPropertyApplyOutcome& OutOutcome)
				{
					if (!Projection.StructuralEdits.IsEmpty())
					{
						auto Diagnostic = ValueFailure(TEXT("sequence_domain_structure_edit_unsupported"),
							TEXT("Sequence semantic workspace import does not support structural edits."), TEXT("/structural_edits"),
							MakeShared<FJsonValueNumber>(Projection.StructuralEdits.Num()), MakeShared<FJsonValueNumber>(0), TargetPackage, TEXT("authored_value"));
						FWorkspaceError LocatedError;
						LocatedError.SetFailure(MoveTemp(Diagnostic), TargetPackage);
						if (Workspace->GetStructuralEditsSource().IsSet()) LocatedError.Locate(Workspace->GetStructuralEditsSource().GetValue());
						OutOutcome.Diagnostic = LocatedError.GetDiagnostic();
						return false;
					}
					UeAgentReflectionAuthoring::FAssetApplyRequest ApplyRequest;
					ApplyRequest.TypeId = Profile->TypeId;
					ApplyRequest.Operation =
						UeAgentApplyInputContract::EOperation::Update;
					ApplyRequest.AssetPath = TargetPackage;
					ApplyRequest.Properties = MoveTemp(Projection.RootObjectProperties);
					for (const auto& Change : Prepared.GetChangeSetDraft().GetChanges())
					{
						if (Change.GetIdentity().GetSemanticId() != Profile->ProfileStateSemanticId) continue;
						TSharedPtr<FJsonValue> Structure;
						UE::UAI::Contract::FDiagnostic Error;
						if (!Change.GetProposedAfter().IsValid()
							|| !StateSemantic::CanonicalJson::TryToJsonValue(
								*Change.GetProposedAfter(), Structure, Error))
						{
							if (Error.IsEmpty()) Error = ValueFailure(TEXT("sequence_domain_structure_value_invalid"),
								TEXT("The prepared sequence structure change has no proposed value."), TEXT("proposed_structure_available"),
								MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TargetPackage);
							Error.SourceKind = TEXT("native_result");
							OutOutcome.Diagnostic = Contract::FDiagnostic::WrapFailure(TEXT("sequence_domain_structure_value_invalid"), Error);
							return false;
						}
						ApplyRequest.ProviderExtension = MakeShared<FJsonObject>();
						ApplyRequest.ProviderExtension->SetField(Profile->ProfileStateField, MoveTemp(Structure));
					}
					ApplyRequest.bSave = true;
					ApplyRequest.bCompactValues = true;
					ApplyRequest.FinalReadback = MoveTemp(OutOutcome.FinalReadback);
					Asset = nullptr;
					UeAgentReflectionAuthoring::FAssetApplyResult ApplyResult;
					const bool bApplied = UeAgentReflectionAuthoring::ApplyAsset(
						ApplyRequest, ApplyResult, RequestId);
					OutOutcome.bApplied =
						bApplied && ApplyResult.Status == TEXT("applied");
					OutOutcome.Phase = MoveTemp(ApplyResult.Phase);
					OutOutcome.Diagnostic = MoveTemp(ApplyResult.Diagnostic);
					return bApplied;
				},
				[&Target](
					FString& OutReadbackPayload,
					FString& OutRevision,
					FWorkspaceError& OutReadError)
				{
					TSharedPtr<FJsonObject> ReadObject;
					FString NativePayload;
					if (!TryRead(
							Target, NativePayload, ReadObject, OutReadError)
						|| !TryReadPersistedPackageRevision(
							Target, OutRevision, OutReadError))
					{
						return false;
					}
					return TryCanonicalizeObject(
						ReadObject, OutReadbackPayload, OutReadError);
				},
				OutPayloadJson,
				OutError);
		}

		bool TryImport(
			const FString& Target,
			const FString& CanonicalParamsJson,
			FString& OutPayloadJson,
			FWorkspaceError& OutError)
		{
			FResolvedDomainHjsonDocument Document;
			if (!TryDecodeResolvedDocument(
					Target,
					CanonicalParamsJson,
					FDomainHjsonDocumentContract(
						TEXT("sequence"), SchemaId, RootType),
					Document,
					OutError))
			{
				return false;
			}
			UE::UAI::HjsonWorkspace::DomainAuthoring::FScopedDomainHjsonDiagnosticSource DiagnosticSource(Document, OutError);
			if (Document.bSemanticWorkspaceDirectory)
			{
				return TryImportSemanticWorkspace(
					Target, Document, OutPayloadJson, OutError);
			}
			bool bPresent = false;
			if (!TryInspectObjectTargetPresence(Target, bPresent))
			{
				return Fail(OutError, ValueFailure(TEXT("sequence_domain_target_invalid"),
					TEXT("The sequence target identity could not be inspected for object presence."), TEXT("target_presence_inspected"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Target), Target);
			}
			if (bPresent)
			{
				return Fail(OutError, ValueFailure(TEXT("sequence_domain_existing_inline_unsupported"),
					TEXT("The sequence target already exists; editing it requires an exported semantic workspace directory."),
					TEXT("target_present"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), Target), Target);
			}
			if (Document.ExpectedRevision != TEXT("missing"))
			{
				auto Diagnostic = ValueFailure(TEXT("sequence_domain_stale_revision"),
					TEXT("Creating an absent sequence requires the expected revision to describe an absent target."), TEXT("/expected_revision"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Target, TEXT("command_input"));
				Diagnostic.ExpectedEvidence = MakeShared<FJsonValueString>(Document.ExpectedRevision);
				Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(TEXT("missing"));
				return Fail(OutError, MoveTemp(Diagnostic), Target);
			}
			if (Document.Root->HasField(TEXT("operation")))
			{
				return Fail(OutError, ValueFailure(TEXT("sequence_domain_create_then_edit_not_atomic"),
					TEXT("Inline sequence creation does not accept an operation field because create-then-edit cannot be applied atomically."),
					TEXT("/root/operation"), Document.Root->TryGetField(TEXT("operation")), MakeShared<FJsonValueString>(TEXT("field omitted")),
					Target, TEXT("authored_value")), Target);
			}
			if (Document.Root->HasField(TEXT("fields")))
			{
				return Fail(OutError, ValueFailure(TEXT("sequence_hjson_fields_retired_use_structure_create_or_directory_edit"),
					TEXT("The retired fields input is unsupported; use root.structure for sequence creation or a semantic workspace directory for editing."),
					TEXT("/root/fields"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), Target, TEXT("authored_value")), Target);
			}
			const TSharedPtr<FJsonObject>* Structure = nullptr;
			if (!Document.Root->TryGetObjectField(TEXT("structure"), Structure)
				|| !Structure || !Structure->IsValid())
			{
				return Fail(OutError, ValueFailure(TEXT("sequence_hjson_create_structure_required"),
					TEXT("Inline sequence creation requires an object at root.structure."), TEXT("/root/structure"),
					Document.Root->TryGetField(TEXT("structure")), MakeShared<FJsonValueString>(TEXT("object")), Target, TEXT("authored_value")), Target);
			}
			const UeAgentReflectionAuthoring::FProfile* Profile = nullptr;
			Contract::FDiagnostic ProfileError;
			if (!TryGetSequenceReflectionProfile(Target, Profile, ProfileError))
				return Fail(OutError, MoveTemp(ProfileError), Target);
			if (!EnsureReflectionAuthoringRegistered(Target, OutError))
			{
				return false;
			}
			const FString TargetPackage =
				UeAgentReflectionAuthoring::NormalizePackagePath(Target);
			FRootPropertyApplyOutcome Outcome;
			Outcome.FinalReadback =
					[&Target, &OutPayloadJson](
						UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure)
					{
						FWorkspaceError ReadError;
						if (!TryReadbackImportPayload(
								TEXT("sequence-domain-provider"), Target, Target,
								nullptr,
								TEXT("sequence_domain_final_readback_payload_invalid"),
								TEXT("sequence_domain_compile_not_settled"),
								[&Target](FString& ReadbackPayload,
									FString& Revision, FWorkspaceError& CallbackError)
								{
									TSharedPtr<FJsonObject> ReadObject;
									FString NativePayload;
									if (!TryRead(Target, NativePayload, ReadObject, CallbackError)
										|| !TryReadPersistedPackageRevision(
											Target, Revision, CallbackError))
									{
										return false;
									}
									return TryCanonicalizeObject(
										ReadObject, ReadbackPayload, CallbackError);
								},
								OutPayloadJson, ReadError))
						{
							Failure.Diagnostic = ReadError.HasErrors() ? ReadError.GetDiagnostic() : UE::UAI::Contract::FDiagnostic::Failure(TEXT("sequence_domain_final_readback_failed"), TEXT("The saved sequence could not be read back."), TEXT("readback"));
							return false;
						}
						return true;
					};
			if (!ApplySequenceStructureAuthoring(
						TargetPackage,
						TEXT("sequence-create-") + FGuid::NewGuid().ToString(
							EGuidFormats::Digits),
						*Structure,
						Outcome))
			{
				if (Outcome.Diagnostic.IsEmpty()) Outcome.Diagnostic = ValueFailure(TEXT("sequence_domain_create_failed"),
					TEXT("Sequence creation failed without providing a diagnostic."), TEXT("sequence_created"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Target);
				return Fail(OutError, MoveTemp(Outcome.Diagnostic), Target);
			}
			return true;
		}
	}

	bool ApplySequenceStructureAuthoring(
		const FString& Target,
		const FString& RequestId,
		const TSharedPtr<FJsonObject>& Structure,
		FRootPropertyApplyOutcome& OutOutcome,
		const UeAgentAuthoringOperationApply::FBackendOptions* BackendOptions)
	{
		OutOutcome.bApplied = false;
		OutOutcome.Phase.Reset();
		OutOutcome.Diagnostic.Reset();
		FWorkspaceError RegistrationError;
		if (!Structure.IsValid())
		{
			OutOutcome.Diagnostic = ValueFailure(TEXT("sequence_structure_missing"),
				TEXT("The sequence apply request has no structure object."), TEXT("structure_object_available"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Target);
			OutOutcome.Phase = TEXT("registration");
			return false;
		}
		if (!EnsureReflectionAuthoringRegistered(Target, RegistrationError))
		{
			OutOutcome.Diagnostic = RegistrationError.GetDiagnostic();
			OutOutcome.Phase = TEXT("registration");
			return false;
		}
		const UeAgentReflectionAuthoring::FProfile* Profile = nullptr;
		if (!TryGetSequenceReflectionProfile(Target, Profile, OutOutcome.Diagnostic)) return false;
		UeAgentReflectionAuthoring::FAssetApplyRequest ApplyRequest;
		ApplyRequest.TypeId = Profile->TypeId;
		ApplyRequest.Operation = UeAgentApplyInputContract::EOperation::Create;
		ApplyRequest.AssetPath =
			UeAgentReflectionAuthoring::NormalizePackagePath(Target);
		ApplyRequest.ProviderExtension = MakeShared<FJsonObject>();
		ApplyRequest.ProviderExtension->SetObjectField(
			Profile->ProfileStateField, Structure.ToSharedRef());
		ApplyRequest.bSave = true;
		ApplyRequest.bCompactValues = true;
		ApplyRequest.FinalReadback = MoveTemp(OutOutcome.FinalReadback);
		if (BackendOptions)
		{
			ApplyRequest.PackageStager = BackendOptions->PackageStager;
			if (BackendOptions->FinalReadback)
			{
				ApplyRequest.FinalReadback = BackendOptions->FinalReadback;
			}
		}
		UeAgentReflectionAuthoring::FAssetApplyResult ApplyResult;
		const bool bApplied = UeAgentReflectionAuthoring::ApplyAsset(
			ApplyRequest, ApplyResult, RequestId);
		OutOutcome.bApplied =
			bApplied && ApplyResult.Status == TEXT("applied");
		OutOutcome.Phase = MoveTemp(ApplyResult.Phase);
		OutOutcome.Diagnostic = MoveTemp(ApplyResult.Diagnostic);
		return bApplied;
	}

	bool TryExecuteContentGraph(
		const FString& CommandId,
		const FString& TargetIdentity,
		const FString& CanonicalParamsJson,
		FString& OutPayloadJson,
		bool& bOutReadOnly,
		FWorkspaceError& OutError,
		FPreparedWorkspaceExport* PreparedExport)
	{
		OutPayloadJson.Reset();
		bOutReadOnly = false;
		OutError.Reset();
		TSharedPtr<FJsonObject> Params;
		UE::UAI::Contract::FDiagnostic JsonError;
		if (!Json::TryParseObject(CanonicalParamsJson, Params, JsonError)
			|| !Params.IsValid())
		{
			if (JsonError.IsEmpty()) JsonError = ValueFailure(TEXT("sequence_domain_params_invalid"),
				TEXT("Sequence command parameters could not be decoded as an object and no diagnostic was provided."), TEXT("parameter_object_available"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TargetIdentity, TEXT("command_input"));
			JsonError.SourceKind = TEXT("command_input");
			return Fail(OutError, MoveTemp(JsonError), TargetIdentity);
		}
		FString Target;
		if (!Params->TryGetStringField(TEXT("target"), Target)
			|| Target != TargetIdentity)
		{
			return Fail(OutError, ValueFailure(TEXT("sequence_domain_target_identity_mismatch"),
				TEXT("The sequence command target must be a string matching the request target identity."), TEXT("/target"),
				Params->TryGetField(TEXT("target")), MakeShared<FJsonValueString>(TargetIdentity), TargetIdentity, TEXT("command_input")), TargetIdentity);
		}
		if (CommandId == TEXT("content_graph.import_workspace"))
		{
			return TryImport(Target, CanonicalParamsJson, OutPayloadJson, OutError);
		}
		if (CommandId == TEXT("content_graph.export_workspace"))
		{
			bOutReadOnly = true;
			return TryExport(Target, Params, OutPayloadJson, OutError, PreparedExport);
		}
		if (CommandId.StartsWith(TEXT("content_graph.")))
		{
			TSharedPtr<FJsonObject> ReadObject;
			FString ReadPayload;
			if (!TryRead(Target, ReadPayload, ReadObject, OutError))
			{
				return false;
			}
			const TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
			Response->SetStringField(TEXT("target"), Target);
			Response->SetStringField(TEXT("root_type"), RootType);
			FString ReadRevision;
			if (!StableRevision(ReadObject, Target, ReadRevision, OutError))
			{
				return false;
			}
			Response->SetStringField(TEXT("revision"), ReadRevision);
			Response->SetBoolField(TEXT("complete"), true);
			Response->SetBoolField(TEXT("has_more"), false);
			Response->SetObjectField(TEXT("query_result"), ReadObject);
			bOutReadOnly = true;
			return TryCanonicalizeObject(Response, OutPayloadJson, OutError);
		}
		return Fail(OutError, ValueFailure(TEXT("sequence_domain_command_not_supported"),
			TEXT("The sequence domain handles content_graph commands."), TEXT("/command"),
			MakeShared<FJsonValueString>(CommandId), MakeShared<FJsonValueString>(TEXT("content_graph.*")), TargetIdentity, TEXT("command_input")), TargetIdentity);
	}
}

#include "DomainAdapters/AudioGraph/UeAgentAudioGraphDomainMechanics.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Composition/HjsonImport/UeAgentSemanticWorkspaceImport.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceIO.h"
#include "Core/HjsonWorkspace/UeAgentGraphAuthoringProjection.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "DomainAdapters/AudioGraph/UeAgentAudioGraphCandidateBindingNormalizer.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"
#include "UObject/ObjectMacros.h"
#include "Domains/Audio/Authoring/UeAgentAudioAuthoringApplyProvider.h"
#include "Domains/Audio/Authoring/UeAgentAudioAuthoringDocuments.h"
#include "Domains/Audio/Authoring/UeAgentAudioAuthoringExport.h"
#include "Domains/Audio/Authoring/UeAgentAudioAuthoringRegistration.h"
#include "Domains/Audio/Authoring/UeAgentAudioAuthoringState.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "OperationOwners/AudioCommands/UeAgentAudioCommandOperationOwner.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::DomainAdapters::AudioGraph
{
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace ContextResource;
	using namespace DomainAdapters::ContentGraphRead;
	using namespace HjsonWorkspace;
	using namespace HjsonWorkspace::DomainAuthoring;

	namespace
	{
		const FString SchemaId = TEXT("uai.audio_graph.edit/1");
		const FString MetaSoundSourceType = TEXT("/Script/MetasoundEngine.MetaSoundSource");
		const FString SoundCueType = TEXT("/Script/Engine.SoundCue");

		Contract::FDiagnostic AudioFact(
			const TCHAR* Code, const FString& Reason, const FString& Target, const FString& Path,
			TSharedPtr<FJsonValue> Actual = nullptr, TSharedPtr<FJsonValue> Expected = nullptr,
			const TCHAR* SourceKind = TEXT(""))
		{
			auto Error = Contract::FDiagnostic::Failure(Code, Reason, TEXT("authoring_apply"));
			Error.TargetPath = Target;
			Error.PropertyPath = Path;
			Error.SemanticPointer = Path;
			Error.Source.InternalJsonPath = Path;
			Error.SourceKind = SourceKind;
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			return Error;
		}

		TSharedPtr<FJsonValue> StringChoices(const TArray<FString>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> Items;
			for (const FString& Value : Values) Items.Add(MakeShared<FJsonValueString>(Value));
			return MakeShared<FJsonValueArray>(MoveTemp(Items));
		}

		TSharedPtr<FJsonValue> RootTypes()
		{
			return StringChoices({MetaSoundSourceType, SoundCueType});
		}

		TSharedPtr<FJsonValue> JsonShape(const TSharedPtr<FJsonValue>& Value)
		{
			if (!Value.IsValid()) return nullptr;
			switch (Value->Type)
			{
			case EJson::Object: return MakeShared<FJsonValueString>(TEXT("object"));
			case EJson::Array: return MakeShared<FJsonValueString>(TEXT("array"));
			case EJson::None: return MakeShared<FJsonValueString>(TEXT("none"));
			default: return Value;
			}
		}

		Contract::FDiagnostic AudioInput(const TCHAR* Code, const FString& Reason,
			const FString& Target, const FString& Path, const TSharedPtr<FJsonValue>& Value,
			const FString& Expected, const TCHAR* SourceKind = TEXT(""))
		{
			auto Error = AudioFact(Code, Reason, Target, Path, JsonShape(Value),
				MakeShared<FJsonValueString>(Expected), SourceKind);
			Error.ActualValuePresent = Value.IsValid();
			Error.ExpectedValuePresent = true;
			if (Value.IsValid() && (Value->Type == EJson::Object || Value->Type == EJson::Array))
				Error.ActualEvidence = Value;
			return Error;
		}

		Contract::FDiagnostic StaleRevision(const FString& Target,
			const FString& Current, const FString& Expected)
		{
			auto Error = AudioFact(TEXT("audio_graph_domain_stale_revision"),
				TEXT("The target revision does not match expected_revision."), Target, TEXT("/expected_revision"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("command_input"));
			Error.ActualEvidence = MakeShared<FJsonValueString>(Current);
			Error.ExpectedEvidence = MakeShared<FJsonValueString>(Expected);
			return Error;
		}

		void LocateSemanticFailure(const FResolvedSemanticWorkspace& Workspace,
			const FString& UnitId, Contract::FDiagnostic& Error)
		{
			const bool bConcreteSource = !Error.Source.SourceFile.IsEmpty() && Error.Source.Line > 0
				&& !Error.Source.SourceFile.StartsWith(TEXT("inline://"))
				&& !Error.Source.SourceFile.StartsWith(TEXT("resolved:"));
			if (!bConcreteSource && Error.SourceKind != TEXT("native_result")
				&& Error.SourceKind != TEXT("command_input"))
			{
				if (Error.Source.SourceUnit.IsEmpty()) Error.Source.SourceUnit = UnitId;
				if (Error.Source.InternalJsonPath.IsEmpty()) Error.Source.InternalJsonPath =
					!Error.SemanticPointer.IsEmpty() ? Error.SemanticPointer : TEXT("/");
				Workspace.LocateDocumentDiagnostic({}, {}, Error, UnitId);
			}
			if (Error.PublicCause)
			{
				auto Cause = *Error.PublicCause;
				LocateSemanticFailure(Workspace, UnitId, Cause);
				Error.PublicCause = MakeShared<Contract::FDiagnostic>(MoveTemp(Cause));
			}
			for (auto& Child : Error.Diagnostics) LocateSemanticFailure(Workspace, UnitId, Child);
		}

		bool Fail(FWorkspaceError& OutError, Contract::FDiagnostic Diagnostic)
		{
			OutError.SetFailure(MoveTemp(Diagnostic));
			return false;
		}
		bool IsSupportedRootType(const FString& Type)
		{
			return Type == MetaSoundSourceType || Type == SoundCueType;
		}

		const UeAgentAudioAuthoring::FProfile* ProfileForRootType(
			const FString& RootType)
		{
			if (RootType == MetaSoundSourceType)
			{
				return UeAgentAudioAuthoring::FindProfile(
					UeAgentAudioAuthoring::EAssetKind::MetaSound);
			}
			if (RootType == SoundCueType)
			{
				return UeAgentAudioAuthoring::FindProfile(
					UeAgentAudioAuthoring::EAssetKind::SoundCue);
			}
			return nullptr;
		}

		bool EnsureAuthoringRegistered(FWorkspaceError& OutError)
		{
			static const TPair<bool, UE::UAI::Contract::FDiagnostic> Registration = []()
			{
				UE::UAI::Contract::FDiagnostic Error;
				const bool bRegistered =
					UeAgentAudioAuthoring::RegisterAuthoring(Error);
				return TPair<bool, UE::UAI::Contract::FDiagnostic>(bRegistered, MoveTemp(Error));
			}();
			if (!Registration.Key)
			{
				return Fail(OutError, Registration.Value);
			}
			return true;
		}

		FString PackageIdentity(const FString& Target)
		{
			return Target.Contains(TEXT("."))
				? FPackageName::ObjectPathToPackageName(Target)
				: Target;
		}

		bool SubmitAudioCommand(
			const FString& CommandId,
			const TSharedPtr<FJsonObject>& Params,
			FString& OutPayload,
			FWorkspaceError& OutError,
			const FResolvedDomainHjsonDocument& Document, const FString& SourcePrefix)
		{
			FScopedDomainHjsonDiagnosticSource NativeSource(Document, OutError, SourcePrefix);
			FString ParamsJson;
			if (!TryCanonicalizeObject(Params, ParamsJson, OutError)) return false;
			const TArray<FString> AuthorityScopes = {
				TEXT("uai:query"),
				TEXT("uai:operate"),
				TEXT("uai:authoring:hjson:import"),
				TEXT("audio_media_graph.write"),
			};
			UE::UAI::Contract::FDiagnostic CommandDiagnostic;
			const FString CorrelationId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
			if (!OperationOwners::AudioCommands::TryExecuteInternal(
					CommandId, ParamsJson,
					Contract::FUeAgentCallerContext(
						TEXT("audio-graph-domain-provider"), AuthorityScopes, CorrelationId),
					OutPayload, CommandDiagnostic))
			{
				SetWorkspaceErrorFromCommandFailure(CommandDiagnostic,
					TEXT("audio_graph_domain_native_command_failed"), CommandId, OutError);
				return false;
			}
			return true;
		}

		bool TryRead(
			const FString& Target,
			FString& OutPayload,
			FString& OutRevision,
			TSharedPtr<FJsonObject>* OutSnapshot,
			FWorkspaceError& OutError)
		{
			UObject* Owner = nullptr;
			FUeAgentContentGraphCapture Capture;
			if (!TryCaptureObject(
					Target,
					TArray<FString>{MetaSoundSourceType, SoundCueType},
					Owner,
					Capture,
					OutError))
			{
				return false;
			}
			const FString RootType = Owner->GetClass()->GetPathName();
			if (!IsSupportedRootType(RootType))
			{
				return Fail(OutError, AudioFact(TEXT("audio_graph_domain_target_type_invalid"),
					TEXT("The audio graph target must be a MetaSoundSource or SoundCue."), Target, TEXT("target.class"),
					MakeShared<FJsonValueString>(RootType), RootTypes(), TEXT("native_result")));
			}
			TSharedPtr<FJsonObject> Snapshot;
			if (!TryBuildCaptureSnapshot(
					Target, RootType, Capture, Snapshot, OutError))
			{
				return false;
			}
			OutRevision = Capture.Revision;
			if (OutSnapshot) *OutSnapshot = Snapshot;
			return TryCanonicalizeObject(Snapshot, OutPayload, OutError);
		}

		bool ValidateMissingTarget(
			const FString& Target,
			FWorkspaceError& OutError)
		{
			bool bPresent = false;
			if (!TryInspectObjectTargetPresence(Target, bPresent))
			{
				return Fail(OutError, AudioFact(TEXT("audio_graph_domain_target_invalid"),
					TEXT("The target presence probe could not resolve this asset reference."), Target, TEXT("/target"),
					MakeShared<FJsonValueString>(Target), MakeShared<FJsonValueString>(TEXT("resolvable asset reference")), TEXT("command_input")));
			}
			if (bPresent)
			{
				return Fail(OutError, AudioFact(TEXT("audio_graph_domain_stale_revision"),
					TEXT("Creating with expected_revision 'missing' requires an absent target."), Target, TEXT("target.present"),
					MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), TEXT("native_result")));
			}
			return true;
		}

		bool TryBuildCreateParams(
			const FString& Target,
			const FString& RootType,
			const TSharedPtr<FJsonObject>& Create,
			TSharedPtr<FJsonObject>& OutNative,
			FWorkspaceError& OutError)
		{
			const FString AssetKind = RootType == MetaSoundSourceType
				? TEXT("metasound_source") : TEXT("sound_cue");
			const FString Package = PackageIdentity(Target);
			const FString ExpectedName =
				FPackageName::GetLongPackageAssetName(Package);
			const FString ExpectedPath =
				FPackageName::GetLongPackagePath(Package);
			if (!Create.IsValid()
				|| ExpectedName.IsEmpty()
				|| ExpectedPath.IsEmpty()
				|| !FPackageName::IsValidLongPackageName(Package))
			{
				auto Error = AudioFact(TEXT("audio_graph_domain_create_contract_invalid"),
					Create.IsValid() ? TEXT("The target must be a valid asset package with a non-empty name and directory.")
						: TEXT("The audio asset create contract must be an object."),
					Target, Create.IsValid() ? TEXT("/target") : TEXT("/root/create"),
					Create.IsValid() ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Package))
						: TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false)),
					MakeShared<FJsonValueString>(Create.IsValid() ? TEXT("valid long package path") : TEXT("object")),
					Create.IsValid() ? TEXT("command_input") : TEXT(""));
				return Fail(OutError, MoveTemp(Error));
			}
			FString Declared;
			if (Create->TryGetStringField(TEXT("asset_kind"), Declared)
				&& Declared != AssetKind)
			{
				return Fail(OutError, AudioFact(TEXT("audio_graph_domain_create_kind_mismatch"),
					TEXT("The declared asset kind does not match the document root type."), Target, TEXT("/root/create/asset_kind"),
					MakeShared<FJsonValueString>(Declared), MakeShared<FJsonValueString>(AssetKind)));
			}
			if (Create->TryGetStringField(TEXT("asset_name"), Declared)
				&& Declared != ExpectedName)
			{
				return Fail(OutError, AudioFact(TEXT("audio_graph_domain_create_name_mismatch"),
					TEXT("The declared asset name does not match the target package."), Target, TEXT("/root/create/asset_name"),
					MakeShared<FJsonValueString>(Declared), MakeShared<FJsonValueString>(ExpectedName)));
			}
			if (Create->TryGetStringField(TEXT("package_path"), Declared))
			{
				while (Declared.EndsWith(TEXT("/")))
				{
					Declared.LeftChopInline(1);
				}
				if (Declared != ExpectedPath)
				{
					return Fail(OutError, AudioFact(TEXT("audio_graph_domain_create_path_mismatch"),
						TEXT("The declared package directory does not match the target package."), Target, TEXT("/root/create/package_path"),
						MakeShared<FJsonValueString>(Declared), MakeShared<FJsonValueString>(ExpectedPath)));
				}
			}
			OutNative = MakeShared<FJsonObject>(*Create);
			OutNative->SetStringField(TEXT("asset_kind"), AssetKind);
			OutNative->SetStringField(TEXT("asset_name"), ExpectedName);
			OutNative->SetStringField(TEXT("package_path"), ExpectedPath);
			return true;
		}

		bool ParseLifecycleMutation(
			const FString& Target,
			const FString& MutationPayload,
			TSharedPtr<FJsonObject>& OutMutation,
			FDomainHjsonImportLifecycleReceipt& OutReceipt,
			FWorkspaceError& OutError)
		{
			return TryReadNativeMutationPayload(Target, MutationPayload, OutMutation, OutError, &OutReceipt);
		}

		const TArray<FString>& AllowedOperations(const FString& RootType)
		{
			static const TArray<FString> MetaSound = {
				TEXT("metasound.graph.interface.mutate"), TEXT("metasound.graph.node.mutate"),
				TEXT("metasound.graph.link.mutate"), TEXT("metasound.graph.default.mutate"),
				TEXT("metasound.graph.compile_settle")};
			static const TArray<FString> SoundCue = {
				TEXT("sound_cue.graph.node.mutate"), TEXT("sound_cue.graph.relation.mutate"),
				TEXT("sound_cue.graph.root.mutate"), TEXT("sound_cue.graph.compile_settle")};
			static const TArray<FString> Empty;
			return RootType == MetaSoundSourceType ? MetaSound : RootType == SoundCueType ? SoundCue : Empty;
		}

		bool IsAllowedOperation(const FString& RootType, const FString& CommandId)
		{
			return AllowedOperations(RootType).Contains(CommandId);
		}

		bool TryImportSemanticWorkspace(
			const FString& Target,
			const FResolvedDomainHjsonDocument& Document,
			FString& OutPayload,
			FWorkspaceError& OutError)
		{
			if (!Document.SemanticWorkspace.IsValid())
			{
				return Fail(OutError, AudioFact(TEXT("audio_graph_semantic_workspace_missing"),
					TEXT("The resolved audio semantic workspace is unavailable."), Target, TEXT("semantic_workspace.available"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("native_result")));
			}
			UObject* Asset = nullptr;
			if (!TryLoadObjectTarget(
					Target, TArray<FString>{MetaSoundSourceType, SoundCueType},
					Asset, OutError))
			{
				return false;
			}
			const FString RootType = Asset
				? Asset->GetClass()->GetPathName() : FString();
			const UeAgentAudioAuthoring::FProfile* Profile =
				ProfileForRootType(RootType);
			if (!Profile)
			{
				return Fail(OutError, AudioFact(TEXT("audio_graph_domain_schema_or_type_invalid"),
					TEXT("The audio graph target must be a MetaSoundSource or SoundCue."), Target, TEXT("target.class"),
					MakeShared<FJsonValueString>(RootType), RootTypes(), TEXT("native_result")));
			}
			if (!EnsureAuthoringRegistered(OutError)) return false;

			const FString TargetPackage = PackageIdentity(Target);
			TArray<UeAgentAuthoringState::FReadRequest> Requests;
			UE::UAI::Contract::FDiagnostic CaptureError;
			if (!UeAgentAudioAuthoring::BuildAssetExportSnapshotReadRequests(
					*Profile, *Asset, Requests, CaptureError))
			{
				OutError.SetFailure(CaptureError);
				return false;
			}
			SelectRootPropertyImportReadRequests(*Document.SemanticWorkspace, Requests);
			UeAgentAuthoringState::FCaptureResult Capture;
			if (!CapturePropertyImportState(*Document.SemanticWorkspace, Requests, Capture))
			{
				OutError.SetFailure(Capture.Diagnostic);
				return false;
			}

			ContextResource::FUeAgentResourceTracker Resources =
				MakeSemanticWorkspaceResourceTracker();
			StateSemantic::FUeAgentCanonicalSnapshotPtr CanonicalSnapshot;
			FAuthoringModelPtr Model;
			if (!TryBuildPropertyImportCurrentCapture(Document.SemanticWorkspace.Get(),
					ResolveTargetCompositionKind(Profile->TargetKind, Profile->TypeId),
					TEXT("llm_authoring"), Capture.Snapshot, Resources,
					CanonicalSnapshot, Model, OutError))
			{
				return false;
			}

			Capture.Snapshot.ReleaseNativeValues();
			const FString TargetObjectPath = ObjectPathFromPackageIdentity(Target);
			const UeAgentAudioAuthoring::EAssetKind AssetKind = Profile->Kind;
			return TryCoordinateSemanticWorkspaceImport(
				Document.SemanticWorkspace.ToSharedRef(), Model.ToSharedRef(),
				CanonicalSnapshot.ToSharedRef(),
				BuildSemanticWorkspaceImportDescriptor(TEXT("audio_graph.semantic_workspace"), *Model),
				TargetObjectPath,
				Resources,
				{
					TEXT("audio-graph-domain-provider"),
					Target,
					TargetObjectPath,
					TEXT("audio-graph-semantic"),
					TEXT("audio_graph_semantic_prepare_failed"),
					TEXT("audio_graph_semantic_apply_failed"),
					TEXT("audio_graph_semantic_readback_payload_invalid"),
					TEXT("audio_graph_domain_compile_not_settled"),
					[Profile, TargetObjectPath](TArray<UeAgentAuthoringState::FReadRequest>& Requests, UE::UAI::Contract::FDiagnostic& Error)
					{
						UObject* Native = StaticLoadObject(UObject::StaticClass(), nullptr, *TargetObjectPath);
						if (!Native)
						{
							Error = AudioFact(TEXT("audio_graph_semantic_readback_target_unresolved"),
								TEXT("The audio asset could not be loaded for semantic readback."), TargetObjectPath, TEXT("target.available"),
								MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("native_result"));
							return false;
						}
						return UeAgentAudioAuthoring::BuildAssetExportSnapshotReadRequests(*Profile, *Native, Requests, Error);
					}},
				[AssetKind, TargetPackage,
					DocumentSemanticId = Profile->DocumentSemanticId, &Asset](
					const FString& RequestId,
					const Composition::HjsonImport::FPreparedAuthoringImport& Prepared,
					Composition::HjsonImport::FSemanticWorkspaceChangeProjection&& Projection,
					FRootPropertyApplyOutcome& OutOutcome)
				{
					TSharedPtr<FJsonObject> DesiredDocument;
					for (const auto& Change : Prepared.GetChangeSetDraft().GetChanges())
					{
						const FString& SemanticId = Change.GetIdentity().GetSemanticId();
						if (SemanticId == TEXT("core.property")) continue;
						FString SourceUnit;
						for (const FString& UnitId : Projection.ChangedUnitIds)
						{
							const auto* Unit = Prepared.GetWorkspace()->GetModel()->FindUnit(UnitId);
							if (Unit && (Unit->GetTargetIdentityKey() == Change.GetIdentity().GetCanonicalKey()
								|| Unit->GetTargetIdentityKey() == Change.GetIdentity().GetSelector(CanonicalAuthoringTargetIdentitySelector)))
							{
								SourceUnit = UnitId;
								break;
							}
						}
						const auto RejectChange = [&](Contract::FDiagnostic Error)
						{
							OutOutcome.Phase = TEXT("semantic_projection");
							if (!SourceUnit.IsEmpty()) LocateSemanticFailure(*Prepared.GetWorkspace(), SourceUnit, Error);
							OutOutcome.Diagnostic = MoveTemp(Error);
							return false;
						};
						if (SemanticId != DocumentSemanticId)
							return RejectChange(AudioFact(TEXT("audio_graph_semantic_document_invalid"),
								TEXT("The changed semantic surface is not the audio document surface."), TargetPackage, TEXT("/"),
								MakeShared<FJsonValueString>(SemanticId), MakeShared<FJsonValueString>(DocumentSemanticId)));
						if (DesiredDocument.IsValid())
							return RejectChange(AudioFact(TEXT("audio_graph_semantic_document_invalid"),
								TEXT("The import contains more than one audio document change."), TargetPackage, TEXT("/"),
								MakeShared<FJsonValueNumber>(2), MakeShared<FJsonValueNumber>(1)));
						if (!Change.GetProposedAfter().IsValid())
							return RejectChange(AudioFact(TEXT("audio_graph_semantic_document_invalid"),
								TEXT("The audio document change requires a proposed document value."), TargetPackage, TEXT("/"),
								MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
						TSharedPtr<FJsonValue> Value;
						if (!StateSemantic::CanonicalJson::TryToJsonValue(*Change.GetProposedAfter(), Value, OutOutcome.Diagnostic))
							return RejectChange(MoveTemp(OutOutcome.Diagnostic));
						if (!Value.IsValid() || Value->Type != EJson::Object)
							return RejectChange(AudioInput(TEXT("audio_graph_semantic_document_invalid"),
								TEXT("The proposed audio document must be an object."), TargetPackage, TEXT("/"), Value, TEXT("object")));
						// Composition already rebuilt the complete document from the selected edits.
						DesiredDocument = Value->AsObject();
						if (AssetKind != UeAgentAudioAuthoring::EAssetKind::SoundCue && Change.GetExpectedBefore().IsValid())
						{
							TMap<FString, StateSemantic::FUeAgentCanonicalValueRef> BeforeFields, AfterFields;
							for (const auto& Field : Change.GetExpectedBefore()->GetObjectItems()) BeforeFields.Add(Field.Key, Field.Value);
							for (const auto& Field : Change.GetProposedAfter()->GetObjectItems()) AfterFields.Add(Field.Key, Field.Value);
							TSharedPtr<FJsonObject> Nodes;
							FWorkspaceError NodeError;
							if (!TryProjectGraphAuthoringNodeChanges(DocumentSemanticId, BeforeFields, AfterFields, Nodes, NodeError, true))
							{
								OutOutcome.Diagnostic = NodeError.GetDiagnostic();
								OutOutcome.Phase = TEXT("semantic_projection");
								return false;
							}
							if (AfterFields.Contains(TEXT("nodes"))) DesiredDocument->SetObjectField(TEXT("nodes"), Nodes);
						}
					}
					const int32 ChangedUnitCount = Projection.ChangedUnitIds.Num();
					UeAgentAudioAuthoring::FAssetApplyRequest ApplyRequest;
					ApplyRequest.Kind = AssetKind;
					ApplyRequest.Operation =
						UeAgentApplyInputContract::EOperation::Update;
					ApplyRequest.AssetPath = TargetPackage;
					ApplyRequest.Document = DesiredDocument;
					ApplyRequest.bCompleteDocument = true;
					ApplyRequest.Properties = MoveTemp(Projection.RootObjectProperties);
					ApplyRequest.bCompactValues = true;
					ApplyRequest.bCompile = true;
					ApplyRequest.bSave = true;
					ApplyRequest.FinalReadback = OutOutcome.FinalReadback;
					Asset = nullptr;
					UeAgentAudioAuthoring::FAssetApplyResult ApplyResult;
					const bool bApplied = UeAgentAudioAuthoring::ApplyAsset(
						ApplyRequest, ApplyResult, RequestId);
					OutOutcome.bApplied = bApplied && ApplyResult.IsApplied();
					OutOutcome.Phase = MoveTemp(ApplyResult.Phase);
					OutOutcome.Diagnostic = MoveTemp(ApplyResult.Diagnostic);
					ApplyResult.Asset = nullptr;
					Asset = nullptr;
					return bApplied;
				},
				[&Target](FString& OutReadbackPayload, FString& OutRevision,
					FWorkspaceError& OutReadError)
				{
					return TryRead(
						Target, OutReadbackPayload, OutRevision, nullptr, OutReadError);
				},
				OutPayload, OutError);
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
						TEXT("audio_graph"),
						SchemaId,
						TArray<FString>{MetaSoundSourceType, SoundCueType}),
					Document,
					OutError))
			{
				return false;
			}
			UE::UAI::HjsonWorkspace::DomainAuthoring::FScopedDomainHjsonDiagnosticSource DiagnosticSource(Document, OutError);
			if (Document.bSemanticWorkspaceDirectory)
			{
				return TryImportSemanticWorkspace(
					Target, Document, OutPayload, OutError);
			}
			FString RootType;
			if (!Document.Root.IsValid()
				|| !Document.Root->TryGetStringField(TEXT("type"), RootType)
				|| !IsSupportedRootType(RootType))
			{
				const auto Value = Document.Root.IsValid() ? Document.Root->TryGetField(TEXT("type")) : nullptr;
				auto Error = AudioInput(TEXT("audio_graph_domain_schema_or_type_invalid"),
					TEXT("The document type must be MetaSoundSource or SoundCue."), Target, TEXT("/root/type"), Value, TEXT("supported audio class"));
				Error.ExpectedValue = RootTypes();
				return Fail(OutError, MoveTemp(Error));
			}
			FString MutationPayload;
			if (Document.ExpectedRevision == TEXT("missing"))
			{
				if (!ValidateMissingTarget(Target, OutError)) return false;
				const TSharedPtr<FJsonObject>* Create = nullptr;
				if (!Document.Root->TryGetObjectField(TEXT("create"), Create)
					|| !Create || !Create->IsValid())
				{
					return Fail(OutError, AudioInput(TEXT("audio_graph_domain_create_contract_required"),
						TEXT("Creating a missing audio target requires a create object."), Target, TEXT("/root/create"),
						Document.Root->TryGetField(TEXT("create")), TEXT("object")));
				}
				if (Document.Root->HasField(TEXT("operation")))
				{
					return Fail(OutError, AudioFact(TEXT("audio_graph_domain_create_then_edit_not_atomic"),
						TEXT("An audio import cannot create and edit the target in the same request."), Target, TEXT("/root/operation"),
						MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false)));
				}
				TSharedPtr<FJsonObject> Native;
				if (!TryBuildCreateParams(
						Target, RootType, *Create, Native, OutError)
					|| !SubmitAudioCommand(
						TEXT("audio_media.asset.create"), Native,
						MutationPayload, OutError, Document, TEXT("/root/create")))
				{
					return false;
				}
			}
			else
			{
				FString BeforePayload;
				FString CurrentRevision;
				if (!TryRead(
						Target, BeforePayload, CurrentRevision, nullptr, OutError))
				{
					return false;
				}
				if (Document.ExpectedRevision != CurrentRevision)
				{
					return Fail(OutError, StaleRevision(Target, CurrentRevision, Document.ExpectedRevision));
				}
				if (Document.Root->HasField(TEXT("create")))
				{
					return Fail(OutError, AudioFact(TEXT("audio_graph_domain_create_requires_missing"),
						TEXT("A create contract is only allowed when expected_revision is 'missing'."), Target, TEXT("/root/create"),
						MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false)));
				}
				const TSharedPtr<FJsonObject>* Operation = nullptr;
				const TSharedPtr<FJsonObject>* NativeParams = nullptr;
				FString CommandId;
				if (!Document.Root->TryGetObjectField(TEXT("operation"), Operation) || !Operation || !Operation->IsValid())
				{
					return Fail(OutError, AudioInput(TEXT("audio_graph_domain_operation_contract_required"),
						TEXT("Editing an audio target requires an operation object."), Target, TEXT("/root/operation"),
						Document.Root->TryGetField(TEXT("operation")), TEXT("object")));
				}
				if (!(*Operation)->TryGetStringField(TEXT("command_id"), CommandId))
				{
					return Fail(OutError, AudioInput(TEXT("audio_graph_domain_operation_contract_required"),
						TEXT("The operation command_id must be a string."), Target, TEXT("/root/operation/command_id"),
						(*Operation)->TryGetField(TEXT("command_id")), TEXT("string")));
				}
				if (!(*Operation)->TryGetObjectField(TEXT("params"), NativeParams) || !NativeParams || !NativeParams->IsValid())
				{
					return Fail(OutError, AudioInput(TEXT("audio_graph_domain_operation_contract_required"),
						TEXT("The operation params must be an object."), Target, TEXT("/root/operation/params"),
						(*Operation)->TryGetField(TEXT("params")), TEXT("object")));
				}
				if (!IsAllowedOperation(RootType, CommandId))
				{
					return Fail(OutError, AudioFact(TEXT("audio_graph_domain_operation_not_supported"),
						FString::Printf(TEXT("This command is not supported for audio root type '%s'."), *RootType),
						Target, TEXT("/root/operation/command_id"), MakeShared<FJsonValueString>(CommandId),
						StringChoices(AllowedOperations(RootType))));
				}
				const TSharedPtr<FJsonObject> Native =
					MakeShared<FJsonObject>(**NativeParams);
				FString NativeTarget;
				if (Native->TryGetStringField(TEXT("target_ref"), NativeTarget)
					&& ObjectPathFromPackageIdentity(NativeTarget)
						!= ObjectPathFromPackageIdentity(Target))
				{
					return Fail(OutError, AudioFact(TEXT("audio_graph_domain_operation_target_mismatch"),
						TEXT("The operation target_ref does not match the import target."), Target, TEXT("/root/operation/params/target_ref"),
						MakeShared<FJsonValueString>(NativeTarget),
MakeShared<FJsonValueString>(ObjectPathFromPackageIdentity(Target))));
				}
				Native->SetStringField(
					TEXT("target_ref"), ObjectPathFromPackageIdentity(Target));
				if (!TryNormalizeAudioGraphCandidateBindings(
						Target, Native, OutError)
					|| !SubmitAudioCommand(
						CommandId, Native, MutationPayload, OutError, Document, TEXT("/root/operation/params")))
				{
					return false;
				}
			}

			TSharedPtr<FJsonObject> Mutation;
			FDomainHjsonImportLifecycleReceipt LifecycleReceipt;
			if (!ParseLifecycleMutation(
					Target, MutationPayload, Mutation,
					LifecycleReceipt, OutError))
			{
				return false;
			}
			FString AfterPayload;
			FString AfterRevision;
			TSharedPtr<FJsonObject> AfterSnapshot;
			if (!TryRead(
					Target, AfterPayload, AfterRevision, &AfterSnapshot, OutError))
			{
				return false;
			}
			if (!TryValidateReadbackDiagnostics(AfterSnapshot, Target,
				TEXT("audio_graph_domain_compile_not_settled"), OutError))
				return false;
			return TryBuildImportSuccessPayload(
				TEXT("audio-graph-domain-provider"),
				Target,
				AfterRevision,
				LifecycleReceipt,
				AfterSnapshot,
				Mutation,
				OutPayload,
				OutError);
		}

		bool TryExport(
			const FString& Target,
			const TSharedPtr<FJsonObject>& Params,
			FString& OutPayload,
			FWorkspaceError& OutError,
			FPreparedWorkspaceExport* PreparedExport)
		{
			FString WorkspacePath;
			if (!Params->TryGetStringField(TEXT("workspace_path"), WorkspacePath)
				|| WorkspacePath.IsEmpty())
			{
				return Fail(OutError, AudioInput(TEXT("audio_graph_domain_export_path_required"),
					TEXT("Export requires a non-empty workspace_path string."), Target, TEXT("/workspace_path"),
					Params->TryGetField(TEXT("workspace_path")), TEXT("non-empty string"), TEXT("command_input")));
			}
			UObject* Asset = nullptr;
			FUeAgentContentGraphCapture Capture;
			if (!TryCaptureObject(
					Target, TArray<FString>{MetaSoundSourceType, SoundCueType},
					Asset, Capture, OutError))
			{
				return false;
			}
			const UeAgentAudioAuthoring::FProfile* Profile = Asset
				? ProfileForRootType(Asset->GetClass()->GetPathName()) : nullptr;
			if (!Profile)
			{
				return Fail(OutError, AudioFact(TEXT("audio_graph_domain_target_type_invalid"),
					TEXT("The audio graph target must be a MetaSoundSource or SoundCue."), Target, TEXT("target.class"),
					MakeShared<FJsonValueString>(Asset ? Asset->GetClass()->GetPathName() : FString()), RootTypes(), TEXT("native_result")));
			}
			if (!EnsureAuthoringRegistered(OutError)) return false;
			UeAgentExportWorkspaceWriter::FWorkspaceResult ExportResult;
			FString ExportError;
			if (!UeAgentAudioAuthoring::ExportAssetWorkspace(*Profile, *Asset, WorkspacePath, TEXT("llm_authoring"), ExportResult, PreparedExport))
			{
				OutError.SetFailure(ExportResult.Diagnostic);
				return false;
			}
			const TSharedPtr<FJsonObject> Result = ExportResult.ToPublicJsonObject();
			return TryCanonicalizeObject(Result, OutPayload, OutError);
		}
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
if (!Json::TryParseObject(CanonicalParamsJson, Params, JsonError))
		{
			return Fail(OutError, MoveTemp(JsonError));
		}
		FString Target;
		if (!Params->TryGetStringField(TEXT("target"), Target) || Target != TargetIdentity)
		{
				return Fail(OutError, AudioInput(TEXT("audio_graph_domain_target_identity_mismatch"),
					TEXT("The target parameter must match the routed audio target."), TargetIdentity, TEXT("/target"),
					Params->TryGetField(TEXT("target")), TargetIdentity, TEXT("command_input")));
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
			FString Revision;
			bOutReadOnly = true;
			return TryRead(Target, OutPayloadJson, Revision, nullptr, OutError);
		}
		return Fail(OutError, AudioFact(TEXT("audio_graph_domain_command_not_supported"),
			TEXT("This audio adapter requires a content_graph command."), Target, TEXT("/command_id"),
			MakeShared<FJsonValueString>(CommandId), MakeShared<FJsonValueString>(TEXT("content_graph.*")), TEXT("command_input")));
	}
}

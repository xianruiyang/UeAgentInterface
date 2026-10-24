#include "QueryOwners/HjsonExternalInput/UeAgentHjsonExternalInputReadback.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Animation/AnimSequence.h"
#include "Core/ArtifactPublisher/UeAgentVerifiedExport.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceResolver.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "DomainAdapters/HjsonExternalInput/UeAgentHjsonExternalInputWorkspace.h"
#include "DomainAdapters/HjsonExternalInput/UeAgentHjsonExternalInputDomain.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/MetaData.h"
#include "Utilities/AuthoringExport/UeAgentExportWorkspaceWriter.h"

namespace UE::UAI::QueryOwners::HjsonExternalInput
{
	using namespace ArtifactPublisher;
	using namespace CommandContract;
	using namespace ContextResource;
	using namespace HjsonWorkspace;
	using namespace StateSemantic;
	using namespace DomainAdapters::HjsonExternalInput;

	namespace
	{
		const FString SchemaId = TEXT("uai.hjson.external_input/1");
		constexpr const TCHAR* MetadataSource =
			TEXT("UAI.FactoryExternalImport.Source");
		constexpr const TCHAR* MetadataSourceSha256 =
			TEXT("UAI.FactoryExternalImport.SourceSha256");
		constexpr const TCHAR* MetadataOptionsFingerprint =
			TEXT("UAI.FactoryExternalImport.OptionsFingerprint");
		constexpr const TCHAR* MetadataOperation =
			TEXT("UAI.FactoryExternalImport.Operation");

		FString Canonical(const TSharedPtr<FJsonObject>& Object)
		{
			FString Result;
			UE::UAI::Contract::FDiagnostic Error;
			return Json::TryCanonicalize(Object, Result, Error)
				? Result : FString();
		}

		FUeAgentResourceTracker MakeResources()
		{
			return FUeAgentResourceTracker();
		}

		bool TryFactoryCommandId(
			const UObject& Target,
			const FString& Operation,
			FString& OutCommandId,
			bool& bOutFbx)
		{
			OutCommandId.Reset();
			bOutFbx = false;
			const FString ClassPath = Target.GetClass()->GetPathName();
			const bool bReimport = Operation == TEXT("reimport");
			if (ClassPath == TEXT("/Script/Engine.Texture2D"))
			{
				OutCommandId = bReimport
					? TEXT("asset.reimport_texture")
					: TEXT("external.texture.import");
			}
			else if (ClassPath == TEXT("/Script/Engine.TextureLightProfile")
				&& !bReimport)
			{
				OutCommandId = TEXT("external.light_profile.import");
			}
			else if (ClassPath == TEXT("/Script/Engine.SoundWave"))
			{
				OutCommandId = bReimport
					? TEXT("asset.reimport_sound_wave")
					: TEXT("external.sound_wave.import");
			}
			else if (ClassPath == TEXT("/Script/SparseVolumeTexture.SparseVolumeTexture"))
			{
				OutCommandId = bReimport
					? TEXT("asset.reimport_sparse_volume_texture")
					: TEXT("external.sparse_volume_texture.import");
			}
			else if (ClassPath == TEXT("/Script/Engine.StaticMesh") && bReimport)
			{
				OutCommandId = TEXT("asset.reimport_static_mesh");
			}
			else if (ClassPath == TEXT("/Script/Engine.SkeletalMesh") && !bReimport)
			{
				OutCommandId = TEXT("external.fbx.skeletal_mesh.import");
				bOutFbx = true;
			}
			else if (ClassPath == TEXT("/Script/Engine.AnimSequence") && !bReimport)
			{
				OutCommandId = TEXT("external.fbx.animation.import");
				bOutFbx = true;
			}
			else if (ClassPath == TEXT("/Script/GeometryCache.GeometryCache") && !bReimport)
			{
				OutCommandId = TEXT("external.geometry_cache.import");
			}
			return !OutCommandId.IsEmpty();
		}

		bool TryReconstructOptions(
			const FString& Fingerprint,
			const bool bReimport,
			const bool bFbx,
			TSharedPtr<FJsonObject>& OutOptions)
		{
			TArray<FString> Fields = {TEXT("replace_existing")};
			if (bFbx)
			{
				Fields.Append({TEXT("import_materials"), TEXT("import_textures"),
					TEXT("create_physics_asset")});
			}
			int32 CombinationCount = 1;
			for (int32 Index = 0; Index < Fields.Num(); ++Index)
			{
				CombinationCount *= 3;
			}
			for (int32 Combination = 0; Combination < CombinationCount; ++Combination)
			{
				int32 Remainder = Combination;
				const TSharedPtr<FJsonObject> Candidate = MakeShared<FJsonObject>();
				bool bApplicable = true;
				for (int32 Index = 0; Index < Fields.Num(); ++Index)
				{
					const int32 State = Remainder % 3;
					Remainder /= 3;
					if (State == 0)
					{
						continue;
					}
					const bool bValue = State == 2;
					if (Fields[Index] == TEXT("replace_existing")
						&& bValue != bReimport)
					{
						bApplicable = false;
						break;
					}
					Candidate->SetBoolField(Fields[Index], bValue);
				}
				if (!bApplicable)
				{
					continue;
				}
				const FString CandidateText = Canonical(Candidate);
				if (!CandidateText.IsEmpty()
					&& Json::Sha256Utf8(CandidateText) == Fingerprint)
				{
					OutOptions = Candidate;
					return true;
				}
			}
			OutOptions.Reset();
			return false;
		}

		bool TryBuildFactoryDocument(
			const FString& TargetIdentity,
			TSharedPtr<FJsonObject>& OutRoot,
			FString& OutSourceDigest,
			FWorkspaceError& OutError)
		{
			OutRoot.Reset();
			auto RejectNative = [&](const FString& Code, const FString& Reason)
			{
				auto Detail = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("external_input_readback"));
				Detail.TargetPath = TargetIdentity;
				Detail.SourceKind = TEXT("native_result");
				OutError.SetFailure(MoveTemp(Detail));
				return false;
			};
			UObject* Target = LoadObject<UObject>(nullptr, *TargetIdentity);
			if (!Target || !Target->GetOutermost())
			{
				return RejectNative(TEXT("hjson_external_input_export_target_missing"),
					!Target ? TEXT("The external-input export target could not be loaded.")
						: TEXT("The external-input export target has no package."));
			}
			FMetaData& Metadata = Target->GetOutermost()->GetMetaData();
			const FString Source = Metadata.GetValue(Target, MetadataSource);
			const FString SourceSha256 = Metadata.GetValue(Target, MetadataSourceSha256);
			const FString OptionsFingerprint = Metadata.GetValue(
				Target, MetadataOptionsFingerprint);
			const FString Operation = Metadata.GetValue(Target, MetadataOperation);
			if (Source.IsEmpty() || SourceSha256.IsEmpty()
				|| OptionsFingerprint.IsEmpty() || Operation.IsEmpty())
			{
				TArray<FString> Missing;
				if (Source.IsEmpty()) Missing.Add(TEXT("source path"));
				if (SourceSha256.IsEmpty()) Missing.Add(TEXT("source content digest"));
				if (OptionsFingerprint.IsEmpty()) Missing.Add(TEXT("import options record"));
				if (Operation.IsEmpty()) Missing.Add(TEXT("import operation"));
				return RejectNative(TEXT("hjson_external_input_provenance_missing"),
					FString::Printf(TEXT("The target's external-input provenance is missing: %s."), *FString::Join(Missing, TEXT(", "))));
			}
			FString CommandId;
			bool bFbx = false;
			if (!TryFactoryCommandId(*Target, Operation, CommandId, bFbx))
			{
				return RejectNative(TEXT("hjson_external_input_factory_route_unresolved"),
					FString::Printf(TEXT("No factory route supports target type '%s' and import operation '%s'."),
						*Target->GetClass()->GetPathName(), *Operation));
			}
			const bool bReimport = Operation == TEXT("reimport");
			TSharedPtr<FJsonObject> Options;
			if (!TryReconstructOptions(
					OptionsFingerprint, bReimport, bFbx, Options)
				|| !Options.IsValid())
			{
				auto Detail = UE::UAI::Contract::FDiagnostic::Failure(TEXT("hjson_external_input_options_unrecoverable"),
					TEXT("The original import options could not be reconstructed from the target's stored provenance."), TEXT("external_input_readback"));
				Detail.TargetPath = TargetIdentity;
				Detail.SourceKind = TEXT("native_result");
				Detail.InternalContext = OptionsFingerprint;
				OutError.SetFailure(MoveTemp(Detail));
				return false;
			}
			const TSharedPtr<FJsonObject> Parameters = MakeShared<FJsonObject>();
			TArray<FString> DependencyIdentities;
			Parameters->SetStringField(
				bReimport ? TEXT("target_ref") : TEXT("destination_ref"),
				TargetIdentity);
			if (CommandId == TEXT("external.fbx.animation.import"))
			{
				const UAnimSequence* Animation = Cast<UAnimSequence>(Target);
				if (!Animation || !Animation->GetSkeleton())
				{
					return RejectNative(TEXT("hjson_external_input_skeleton_readback_missing"),
						!Animation ? FString::Printf(TEXT("An animation import requires an AnimSequence target; the actual type is '%s'."), *Target->GetClass()->GetPathName())
							: TEXT("The animation target has no skeleton to restore in the exported input."));
				}
				Parameters->SetStringField(
					TEXT("skeleton_ref"), Animation->GetSkeleton()->GetPathName());
				DependencyIdentities.Add(Animation->GetSkeleton()->GetPathName());
			}
			const TSharedPtr<FJsonObject> SourceInfo = MakeShared<FJsonObject>();
			SourceInfo->SetStringField(TEXT("asset_class"),
				Target->GetClass()->GetPathName());
			SourceInfo->SetStringField(TEXT("import_operation"), Operation);
			// The fingerprint remains in native provenance, never in editable HJSON.
			SourceInfo->SetArrayField(TEXT("generated_assets"), {
				MakeShared<FJsonValueString>(TargetIdentity)});
			TArray<TSharedPtr<FJsonValue>> Dependencies;
			for (const FString& Identity : DependencyIdentities)
			{
				Dependencies.Add(MakeShared<FJsonValueString>(Identity));
			}
			SourceInfo->SetArrayField(TEXT("dependencies"), MoveTemp(Dependencies));
			const TSharedPtr<FJsonObject> SourceObject = MakeShared<FJsonObject>();
			SourceObject->SetStringField(TEXT("identity"), Source);
			SourceObject->SetStringField(TEXT("format"),
				FPaths::GetExtension(Source).ToLower());
			OutSourceDigest = SourceSha256;
			SourceObject->SetObjectField(TEXT("source_info"), SourceInfo);
			const TSharedPtr<FJsonObject> TargetPolicy = MakeShared<FJsonObject>();
			TargetPolicy->SetStringField(TEXT("command_id"), CommandId);
			TargetPolicy->SetObjectField(TEXT("parameters"), Parameters);
			const TSharedPtr<FJsonObject> ExternalInput = MakeShared<FJsonObject>();
			ExternalInput->SetObjectField(TEXT("source"), SourceObject);
			ExternalInput->SetObjectField(TEXT("options"), Options);
			ExternalInput->SetObjectField(TEXT("target_policy"), TargetPolicy);
			const TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("type"), TEXT("uai.external_input.factory"));
			Root->SetObjectField(TEXT("external_input"), ExternalInput);
			// An existing texture/wave source is edited via its native reimport contract.
			FString ReimportCommand;
			bool bReimportFbx = false;
			if (TryFactoryCommandId(*Target, TEXT("reimport"), ReimportCommand, bReimportFbx))
			{
				TargetPolicy->SetStringField(TEXT("command_id"), ReimportCommand);
				Parameters->RemoveField(TEXT("destination_ref"));
				Parameters->SetStringField(TEXT("target_ref"), TargetIdentity);
				Options->SetBoolField(TEXT("replace_existing"), true);
			}
			OutRoot = Root;
			return true;
		}

		class FVerifiedExternalInputExportSource final : public IVerifiedExportWorkspaceSource
		{
		public:
			FVerifiedExternalInputExportSource(FString InTarget, FAuthoringModelRef InModel)
				: Target(MoveTemp(InTarget)), Model(MoveTemp(InModel)) {}
			virtual bool TryFreezeTarget(FUeAgentStateRevision& OutRevision, FArtifactPublishError& OutError) override
			{ return ReadRevision(OutRevision, OutError); }
			virtual bool TryCaptureWorkspace(const FUeAgentStateRevision& FrozenRevision,
				FExportWorkspacePlanPtr& OutPlan, FExportSemanticSignature& OutSemantic,
				FArtifactPublishError& OutError) override
			{
				auto Resources = MakeResources();
				FWorkspaceError Error;
				if (!TryBuildCompositionExportPlan(Model,
					{Target, FrozenRevision, Model->GetSemanticFingerprint()}, {}, Resources, OutPlan, Error))
				{
					OutError = Error.GetDiagnostic();
					return false;
				}
				OutSemantic = Signature(Model->GetSemanticFingerprint());
				return true;
			}
			virtual bool TryRebuildStagedWorkspace(const FString& Path, EWorkspaceKind,
				const FString&, FUeAgentResourceTracker& Resources,
				FExportSemanticSignature& OutSemantic, FArtifactPublishError& OutError) override
			{
				FResolvedSemanticWorkspacePtr Workspace;
				FWorkspaceError Error;
				if (!TryResolveSemanticWorkspace(FWorkspaceSource::Directory(Path),
					ESemanticWorkspaceMode::State, Resources, Workspace, Error))
				{
					OutError = Error.GetDiagnostic();
					return false;
				}
				OutSemantic = Signature(Workspace->GetModel()->GetSemanticFingerprint());
				return true;
			}
			virtual bool TryReadCurrentTargetRevision(FUeAgentStateRevision& OutRevision,
				FArtifactPublishError& OutError) override { return ReadRevision(OutRevision, OutError); }
		private:
			FExportSemanticSignature Signature(const FString& Fingerprint) const
			{ return FExportSemanticSignature(Fingerprint, Fingerprint, Fingerprint, Fingerprint, Fingerprint); }
			bool ReadRevision(FUeAgentStateRevision& OutRevision, FArtifactPublishError& OutError) const
			{
				FString Revision;
				FWorkspaceError Error;
				const FSemanticUnit* TargetUnit = Model->FindUnit(TEXT("target"));
				const FSemanticUnit* External = Model->FindUnit(TEXT("external"));
				TSharedPtr<FJsonObject> Provenance;
				FString Source; UE::UAI::Contract::FDiagnostic ParseError;
				if (!TargetUnit || !External)
				{
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("hjson_external_input_provenance_missing"),
						!TargetUnit ? TEXT("The prepared workspace has no target record.") : TEXT("The prepared workspace has no external-input record."), TEXT("artifact_publish"));
					OutError.TargetPath = Target;
					OutError.SourceKind = TEXT("native_result");
					return false;
				}
				if (!Json::TryParseObject(External->GetSourceProvenance(), Provenance, ParseError))
				{
					OutError = MoveTemp(ParseError);
					OutError.TargetPath = Target;
					OutError.SourceKind = TEXT("native_result");
					return false;
				}
				if (!Provenance->TryGetStringField(TEXT("source"), Source))
				{
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("hjson_external_input_provenance_missing"),
						TEXT("The prepared external-input provenance has no readable source path."), TEXT("artifact_publish"));
					OutError.TargetPath = Target;
					OutError.SourceKind = TEXT("native_result");
					return false;
				}
				if (!TryReadExternalInputTargetRevision(Target, Revision, Error, TargetUnit->GetSemanticTypeRef(), Source))
				{
					OutError = Error.GetDiagnostic();
					return false;
				}
				if (Revision != Model->GetBaseRevision())
				{
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("hjson_external_input_target_changed"),
						TEXT("The external-input target changed after the workspace was prepared."), TEXT("artifact_publish"));
					OutError.TargetPath = Target;
					OutError.SourceKind = TEXT("native_result");
					OutError.ActualEvidence = MakeShared<FJsonValueString>(Revision);
					OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Model->GetBaseRevision());
					return false;
				}
				OutRevision = FUeAgentStateRevision(TEXT("external-input"), Revision, 1);
				return true;
			}
			FString Target;
			FAuthoringModelRef Model;
		};
	}

	const TArray<FString>& GetHjsonExternalInputReadbackFields()
	{
		static const TArray<FString> Fields = {
			TEXT("source.identity"),
			TEXT("source.format"),
			TEXT("source.source_info"),
			TEXT("options"),
			TEXT("target_policy"),
			TEXT("generated_assets"),
			TEXT("dependencies"),
		};
		return Fields;
	}

	bool HasHjsonExternalInputProvenance(const FString& TargetIdentity)
	{
		UObject* Target = LoadObject<UObject>(nullptr, *TargetIdentity);
		if (!Target || !Target->GetOutermost())
		{
			return false;
		}
		FMetaData& Metadata = Target->GetOutermost()->GetMetaData();
		return !FString(Metadata.GetValue(Target, MetadataSource)).IsEmpty()
			&& !FString(Metadata.GetValue(Target, MetadataSourceSha256)).IsEmpty()
			&& !FString(Metadata.GetValue(Target, MetadataOptionsFingerprint)).IsEmpty()
			&& !FString(Metadata.GetValue(Target, MetadataOperation)).IsEmpty();
	}

	bool TryExportHjsonExternalInputWorkspace(
		const FString& TargetIdentity,
		const FString& CanonicalParamsJson,
		FString& OutPayloadJson,
		FWorkspaceError& OutError)
	{
		OutPayloadJson.Reset();
		OutError.Reset();
		TSharedPtr<FJsonObject> Params;
		UE::UAI::Contract::FDiagnostic JsonError;
		if (!Json::TryParseObject(CanonicalParamsJson, Params, JsonError)
			|| !Params.IsValid())
		{
			OutError.SetFailure(JsonError, FString());
			return false;
		}
		FString WorkspacePath;
		if (!Params->TryGetStringField(TEXT("workspace_path"), WorkspacePath)
			|| WorkspacePath.TrimStartAndEnd().IsEmpty())
		{
			auto Detail = UE::UAI::Contract::FDiagnostic::Failure(TEXT("hjson_external_input_export_path_required"),
				TEXT("External-input workspace export requires a non-empty workspace_path."), TEXT("external_input_export"));
			Detail.TargetPath = TargetIdentity;
			Detail.SourceKind = TEXT("command_input");
			Detail.Source.JsonPath = TEXT("/workspace_path");
			Detail.ActualValue = Params->TryGetField(TEXT("workspace_path"));
			Detail.ActualValuePresent = Detail.ActualValue.IsValid();
			Detail.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-empty workspace path"));
			OutError.SetFailure(MoveTemp(Detail));
			return false;
		}
		TSharedPtr<FJsonObject> Root;
		FString ExpectedSourceDigest;
		if (Params->HasField(TEXT("source")))
		{
			// Source has already been normalized by the common public source owner.
			DomainAuthoring::FResolvedDomainHjsonDocument Resolved;
			TArray<FString> Families;
			for (const auto& Family : GetHjsonExternalInputFamilyContracts()) Families.Add(Family.RootTypeIdentity);
			if (!DomainAuthoring::TryDecodeResolvedDocument(TargetIdentity, CanonicalParamsJson,
				{TEXT("external_input"), SchemaId, Families}, Resolved, OutError)) return false;
			if (Resolved.bSemanticWorkspaceDirectory)
			{
				auto Detail = UE::UAI::Contract::FDiagnostic::Failure(TEXT("hjson_external_input_prepare_requires_document"),
					TEXT("External-input preparation requires a source document; the supplied source is an exported workspace directory."), TEXT("external_input_export"));
				Detail.TargetPath = TargetIdentity;
				Detail.SourceKind = TEXT("command_input");
				Detail.Source.JsonPath = TEXT("/source");
				OutError.SetFailure(MoveTemp(Detail));
				return false;
			}
			Root = Resolved.Root;
		}
		else if (!TryBuildFactoryDocument(TargetIdentity, Root, ExpectedSourceDigest, OutError)) return false;
		FString BaseRevision;
		FString RootType, SourceIdentity;
		const TSharedPtr<FJsonObject>* External = nullptr;
		const TSharedPtr<FJsonObject>* Source = nullptr;
		if (!Root.IsValid() || !Root->TryGetStringField(TEXT("type"), RootType)
			|| !Root->TryGetObjectField(TEXT("external_input"), External)
			|| !(*External)->TryGetObjectField(TEXT("source"), Source)
			|| !(*Source)->TryGetStringField(TEXT("identity"), SourceIdentity))
		{
			auto Detail = UE::UAI::Contract::FDiagnostic::Failure(TEXT("hjson_external_input_readable_source_required"),
				!Root.IsValid() ? TEXT("The external-input export has no root object.")
				: RootType.IsEmpty() ? TEXT("The external-input root type is missing or is not a string.")
				: !External ? TEXT("The external-input body is missing or is not an object.")
				: !Source ? TEXT("The external-input source is missing or is not an object.")
				: TEXT("The external-input source identity is missing or is not a string."), TEXT("external_input_export"));
			Detail.TargetPath = TargetIdentity;
			Detail.Source.InternalJsonPath = !Root.IsValid() ? TEXT("/root") : RootType.IsEmpty() ? TEXT("/root/type")
				: !External ? TEXT("/root/external_input") : !Source ? TEXT("/root/external_input/source")
				: TEXT("/root/external_input/source/identity");
			OutError.SetFailure(MoveTemp(Detail));
			return false;
		}
		FAuthoringModelPtr Model;
		if (!TryReadExternalInputTargetRevision(TargetIdentity, BaseRevision, OutError, RootType, SourceIdentity)
			|| !TryBuildExternalInputWorkspaceModel(TargetIdentity, Root, BaseRevision,
				ExpectedSourceDigest, Model, OutError)) return false;

		FUeAgentResourceTracker Resources = MakeResources();
		FArtifactPublishReceipt Receipt;
		FArtifactPublishError PublishError;
		if (!TryExportVerifiedWorkspace(
				FVerifiedExportRequest(
					MakeShared<FVerifiedExternalInputExportSource>(
						TargetIdentity, Model.ToSharedRef()),
					WorkspacePath,
					TEXT("hjson-external-input-domain-owner")),
				Resources,
				Receipt,
				PublishError))
		{
			OutError.SetFailure(PublishError);
			return false;
		}
		const TSharedRef<FJsonObject> Payload =
			UeAgentExportWorkspaceWriter::MakePublicWorkspaceSuccessResult(Receipt.GetCleanupWarnings());
		UE::UAI::Contract::FDiagnostic ResultEncodingError;
		if (!Json::TryCanonicalize(Payload, OutPayloadJson, ResultEncodingError))
		{
			OutError.SetFailure(Receipt.MakeResultEncodingFailure(ResultEncodingError));
			return false;
		}
		return true;
	}
}

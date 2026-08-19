#include "QueryOwners/HjsonExternalInput/UeAgentHjsonExternalInputReadback.h"

#include "Animation/AnimSequence.h"
#include "Core/ArtifactPublisher/UeAgentVerifiedExport.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/MetaData.h"

namespace UE::UAI::QueryOwners::HjsonExternalInput
{
	using namespace ArtifactPublisher;
	using namespace CommandContract;
	using namespace ContextResource;
	using namespace HjsonWorkspace;
	using namespace StateSemantic;

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
			FString Error;
			return Json::TryCanonicalize(Object, Result, Error)
				? Result : FString();
		}

		FUeAgentResourceTracker MakeResources()
		{
			return FUeAgentResourceTracker(FUeAgentResourceBudget({
				{EUeAgentResourceDimension::InputBytes, 16 * 1024 * 1024},
				{EUeAgentResourceDimension::OutputBytes, 16 * 1024 * 1024},
				{EUeAgentResourceDimension::StringBytes, 16 * 1024 * 1024},
				{EUeAgentResourceDimension::Nodes, 4096},
				{EUeAgentResourceDimension::Items, 65536},
				{EUeAgentResourceDimension::ReferenceEdges, 65536},
				{EUeAgentResourceDimension::WorkUnits, 262144},
				{EUeAgentResourceDimension::EstimatedMemoryBytes, 128 * 1024 * 1024},
			}));
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
			FString& OutDocument,
			FWorkspaceError& OutError)
		{
			OutDocument.Reset();
			UObject* Target = LoadObject<UObject>(nullptr, *TargetIdentity);
			if (!Target || !Target->GetOutermost())
			{
				OutError.Set(TEXT("hjson_external_input_export_target_missing"), TargetIdentity);
				return false;
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
				OutError.Set(TEXT("hjson_external_input_provenance_missing"), TargetIdentity);
				return false;
			}
			FString CommandId;
			bool bFbx = false;
			if (!TryFactoryCommandId(*Target, Operation, CommandId, bFbx))
			{
				OutError.Set(TEXT("hjson_external_input_factory_route_unresolved"),
					Target->GetClass()->GetPathName());
				return false;
			}
			const bool bReimport = Operation == TEXT("reimport");
			TSharedPtr<FJsonObject> Options;
			if (!TryReconstructOptions(
					OptionsFingerprint, bReimport, bFbx, Options)
				|| !Options.IsValid())
			{
				OutError.Set(TEXT("hjson_external_input_options_unrecoverable"),
					OptionsFingerprint);
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
					OutError.Set(TEXT("hjson_external_input_skeleton_readback_missing"),
						TargetIdentity);
					return false;
				}
				Parameters->SetStringField(
					TEXT("skeleton_ref"), Animation->GetSkeleton()->GetPathName());
				DependencyIdentities.Add(Animation->GetSkeleton()->GetPathName());
			}
			const TSharedPtr<FJsonObject> SourceInfo = MakeShared<FJsonObject>();
			SourceInfo->SetStringField(TEXT("asset_class"),
				Target->GetClass()->GetPathName());
			SourceInfo->SetStringField(TEXT("import_operation"), Operation);
			SourceInfo->SetStringField(TEXT("options_fingerprint"), OptionsFingerprint);
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
			SourceObject->SetStringField(TEXT("content_sha256"), SourceSha256);
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
			const TSharedPtr<FJsonObject> Document = MakeShared<FJsonObject>();
			Document->SetStringField(TEXT("schema"), SchemaId);
			Document->SetObjectField(TEXT("root"), Root);
			OutDocument = Canonical(Document);
			if (OutDocument.IsEmpty())
			{
				OutError.Set(TEXT("hjson_external_input_export_document_invalid"), TargetIdentity);
				return false;
			}
			return true;
		}

		class FVerifiedExternalInputExportSource final
			: public IVerifiedExportWorkspaceSource
		{
		public:
			FVerifiedExternalInputExportSource(FString InTarget, FString InDocument)
				: Target(MoveTemp(InTarget)), Document(MoveTemp(InDocument)) {}

			virtual bool TryFreezeTarget(
				FUeAgentStateRevision& OutRevision,
				FArtifactPublishError& OutError) override
			{
				return ReadRevision(OutRevision, OutError);
			}

			virtual bool TryCaptureWorkspace(
				const FUeAgentStateRevision& FrozenRevision,
				FExportWorkspacePlanPtr& OutPlan,
				FExportSemanticSignature& OutSemantic,
				FArtifactPublishError& OutError) override
			{
				const FTCHARToUTF8 Utf8(*Document);
				TArray<uint8> Bytes;
				Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
				const FString FileHash = Json::Sha256Utf8(Document);
				OutSemantic = Signature(FileHash);
				OutPlan = MakeShared<FExportWorkspacePlan>(
					EWorkspaceKind::Directory,
					TEXT("asset.hjson"),
					Target,
					FrozenRevision,
					Json::Sha256Utf8(Target + TEXT("|") + FileHash),
					TArray<FExportFilePlan>{
						FExportFilePlan(TEXT("asset.hjson"), Bytes, FileHash)},
					Json::Sha256Utf8(FileHash + TEXT("|")
						+ FrozenRevision.GetGeneration()),
					FMath::Max<int64>(1, Bytes.Num() / 4));
				return true;
			}

			virtual bool TryRebuildStagedWorkspace(
				const FString& StagedWorkspacePath,
				const EWorkspaceKind WorkspaceKind,
				const FString& EntryFile,
				FUeAgentResourceTracker& Resources,
				FExportSemanticSignature& OutSemantic,
				FArtifactPublishError& OutError) override
			{
				FString StagedDocument;
				const FString Path = StagedWorkspacePath / EntryFile;
				if (!FFileHelper::LoadFileToString(StagedDocument, *Path))
				{
					OutError.Set(TEXT("hjson_external_input_export_rebuild_failed"), Path);
					return false;
				}
				OutSemantic = Signature(Json::Sha256Utf8(StagedDocument));
				return true;
			}

			virtual bool TryReadCurrentTargetRevision(
				FUeAgentStateRevision& OutRevision,
				FArtifactPublishError& OutError) override
			{
				return ReadRevision(OutRevision, OutError);
			}

		private:
			FExportSemanticSignature Signature(const FString& FileHash) const
			{
				return FExportSemanticSignature(
					Json::Sha256Utf8(TEXT("asset.hjson")),
					Json::Sha256Utf8(TEXT("hjson-external-input-reference-closure|") + FileHash),
					FileHash,
					Json::Sha256Utf8(SchemaId),
					Json::Sha256Utf8(TEXT("hjson-external-input-business|") + FileHash));
			}

			bool ReadRevision(
				FUeAgentStateRevision& OutRevision,
				FArtifactPublishError& OutError) const
			{
				UObject* Object = LoadObject<UObject>(nullptr, *Target);
				if (!Object || !Object->GetOutermost())
				{
					OutError.Set(TEXT("hjson_external_input_export_target_missing"), Target);
					return false;
				}
				OutRevision = FUeAgentStateRevision(
					TEXT("hjson-external-input-export"),
					Object->GetOutermost()->GetPersistentGuid().ToString(
						EGuidFormats::Digits),
					1);
				return true;
			}

			FString Target;
			FString Document;
		};
	}

	const TArray<FString>& GetHjsonExternalInputReadbackFields()
	{
		static const TArray<FString> Fields = {
			TEXT("source.identity"),
			TEXT("source.format"),
			TEXT("source.content_sha256"),
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
		FString JsonError;
		if (!Json::TryParseObject(CanonicalParamsJson, Params, JsonError)
			|| !Params.IsValid())
		{
			OutError.Set(TEXT("hjson_external_input_export_params_invalid"), JsonError);
			return false;
		}
		FString WorkspacePath;
		if (!Params->TryGetStringField(TEXT("workspace_path"), WorkspacePath)
			|| WorkspacePath.TrimStartAndEnd().IsEmpty())
		{
			OutError.Set(TEXT("hjson_external_input_export_path_required"), TargetIdentity);
			return false;
		}
		FString Document;
		if (!TryBuildFactoryDocument(TargetIdentity, Document, OutError))
		{
			return false;
		}
		FUeAgentResourceTracker Resources = MakeResources();
		FArtifactPublishReceipt Receipt;
		FArtifactPublishError PublishError;
		if (!TryExportVerifiedWorkspace(
				FVerifiedExportRequest(
					MakeShared<FVerifiedExternalInputExportSource>(
						TargetIdentity, Document),
					WorkspacePath,
					TEXT("hjson-external-input-domain-owner")),
				Resources,
				Receipt,
				PublishError))
		{
			OutError.Set(PublishError.GetCode(), PublishError.GetPath(),
				PublishError.GetDetail());
			return false;
		}
		const TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("command_id"),
			TEXT("content_graph.export_workspace"));
		Payload->SetStringField(TEXT("status"), TEXT("published"));
		Payload->SetStringField(TEXT("workspace_path"), Receipt.GetFinalPath());
		Payload->SetStringField(TEXT("entry_file"), Receipt.GetEntryFile());
		Payload->SetStringField(TEXT("plan_fingerprint"), Receipt.GetPlanFingerprint());
		Payload->SetStringField(TEXT("inventory_hash"), Receipt.GetInventoryHash());
		const TSharedPtr<FJsonObject> Semantic = MakeShared<FJsonObject>();
		for (const TPair<FString, FString>& Field
			: Receipt.GetSemanticVerificationFields())
		{
			Semantic->SetStringField(Field.Key, Field.Value);
		}
		Payload->SetObjectField(TEXT("semantic_verification"), Semantic);
		OutPayloadJson = Canonical(Payload);
		return !OutPayloadJson.IsEmpty();
	}
}

#include "DomainAdapters/HjsonExternalInput/UeAgentHjsonExternalInputWorkspace.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceIO.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "DomainAdapters/HjsonExternalInput/UeAgentHjsonExternalInputDomain.h"
#include "DomainAdapters/ExternalImport/Localization/UeAgentLocalizationImportProvider.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/Archive.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END

namespace UE::UAI::DomainAdapters::HjsonExternalInput
{
	using namespace HjsonWorkspace;
	using namespace ContextResource;
	using namespace StateSemantic;
	using namespace CommandContract;

	namespace
	{
		FUeAgentResourceTracker Resources()
		{
			return FUeAgentResourceTracker();
		}

		FString SourcePath(const FString& Source)
		{
			FString Path = FPaths::IsRelative(Source)
				? FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), Source)
				: FPaths::ConvertRelativePathToFull(Source);
			FPaths::NormalizeFilename(Path);
			return Path;
		}
	}

	const FString& GetExternalInputWorkspaceContract()
	{
		static const FString Id = TEXT("uai.hjson.external_input.workspace/1");
		return Id;
	}

	bool TryReadExternalInputSource(const FString& Source, FString& OutPath,
		FString& OutDigest, FWorkspaceError& OutError)
	{
		OutPath = SourcePath(Source);
		OutDigest.Reset();
		const FFileStatData Before = IFileManager::Get().GetStatData(*OutPath);
		TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*OutPath));
		if (Source.IsEmpty() || !Before.bIsValid || Before.bIsDirectory || !Reader)
		{
			OutError.SetFailure(Contract::FDiagnostic::Failure(TEXT("hjson_external_input_source_unavailable"),
				FString::Printf(TEXT("External source '%s' %s."), *Source,
					Source.IsEmpty() ? TEXT("is empty") : !Before.bIsValid ? TEXT("does not exist")
					: Before.bIsDirectory ? TEXT("is a directory") : TEXT("could not be opened for reading")), TEXT("input_decode")));
			return false;
		}
		SHA256_CTX Context;
		SHA256_Init(&Context);
		TArray<uint8> Buffer;
		Buffer.SetNumUninitialized(64 * 1024);
		int64 Remaining = Reader->TotalSize();
		while (Remaining > 0 && !Reader->IsError())
		{
			const int64 Count = FMath::Min<int64>(Remaining, Buffer.Num());
			Reader->Serialize(Buffer.GetData(), Count);
			SHA256_Update(&Context, Buffer.GetData(), static_cast<size_t>(Count));
			Remaining -= Count;
		}
		const bool bRead = !Reader->IsError() && Reader->Close();
		const FFileStatData After = IFileManager::Get().GetStatData(*OutPath);
		if (!bRead || !After.bIsValid || Before.FileSize != After.FileSize
			|| Before.ModificationTime != After.ModificationTime)
		{
			OutError.SetFailure(Contract::FDiagnostic::Failure(TEXT("hjson_external_input_source_changed"),
				FString::Printf(TEXT("External source '%s' %s."), *Source,
					!bRead ? TEXT("could not be read completely") : TEXT("changed while its digest was being read")), TEXT("input_decode")));
			return false;
		}
		uint8 Digest[SHA256_DIGEST_LENGTH];
		SHA256_Final(Digest, &Context);
		OutDigest = TEXT("sha256:") + BytesToHex(Digest, SHA256_DIGEST_LENGTH).ToLower();
		return true;
	}

	bool TryReadExternalInputTargetRevision(const FString& Target, FString& OutRevision, FWorkspaceError& OutError,
		const FString& RootType, const FString& Source)
	{
		if (RootType == TEXT("uai.external_input.localization"))
		{
			using namespace ExternalImport::Localization;
			TSharedPtr<ILocalizationImportProvider> Provider;
			TSharedPtr<ILocalizationImportSession> Prepared;
			FLocalizationImportError Error;
			UE::UAI::Contract::FDiagnostic CreationError;
			FLocalizationImportSpec Spec;
			Spec.RequestId = TEXT("hjson-source-preparation");
			Spec.TargetRef = Target;
			Spec.SourceRef = Source;
			if (!TryCreateLocalizationImportProvider(Provider, CreationError)
				|| !Provider->Prepare(Spec, Prepared, Error) || !Prepared.IsValid())
			{
				OutError.SetFailure(CreationError.IsValid() ? CreationError : Error.IsValid() ? Error
					: UE::UAI::Contract::FDiagnostic::Failure(TEXT("localization_import_prepare_failed"),
						TEXT("The localization importer did not produce a prepared session."), TEXT("external_import")), Target);
				return false;
			}
			// The native importer owns the manifest/options/output-file snapshot.
			// Preparing it captures that baseline without running import or persistence.
			OutRevision = Prepared->GetExpectedRevision();
			return true;
		}
		UObject* Object = LoadObject<UObject>(nullptr, *Target);
		if (!Object)
		{
			OutRevision = TEXT("missing");
			return true;
		}
		if (Object->GetOutermost()->IsDirty())
		{
			auto Detail = UE::UAI::Contract::FDiagnostic::Failure(TEXT("hjson_external_input_target_dirty"),
				TEXT("The target has unsaved changes; save it before capturing the external-input revision."), TEXT("external_import"));
			Detail.TargetPath = Target;
			Detail.SourceKind = TEXT("native_result");
			OutError.SetFailure(MoveTemp(Detail));
			return false;
		}
		return DomainAuthoring::TryReadPersistedPackageRevision(Target, OutRevision, OutError);
	}

	bool TryBuildExternalInputWorkspaceModel(const FString& Target,
		const TSharedPtr<FJsonObject>& Root, const FString& BaseRevision,
		const FString& ExpectedSourceDigest, FAuthoringModelPtr& OutModel,
		FWorkspaceError& OutError)
	{
		OutModel.Reset();
		FString RootType;
		const TSharedPtr<FJsonObject>* External = nullptr;
		const TSharedPtr<FJsonObject>* Source = nullptr;
		FString Identity;
		auto RejectSource = [&](const FString& Reason, const FString& Pointer,
			const TSharedPtr<FJsonValue>& Actual, const FString& Expected)
		{
			auto Detail = UE::UAI::Contract::FDiagnostic::Failure(TEXT("hjson_external_input_readable_source_required"),
				Reason, TEXT("external_import"));
			Detail.TargetPath = Target;
			Detail.Source.InternalJsonPath = Pointer;
			Detail.ActualValue = Actual;
			Detail.ActualValuePresent = Actual.IsValid();
			Detail.ExpectedValue = MakeShared<FJsonValueString>(Expected);
			OutError.SetFailure(MoveTemp(Detail));
			return false;
		};
		if (!Root.IsValid())
			return RejectSource(TEXT("The external-input document has no root object."), TEXT("/root"), nullptr, TEXT("object"));
		if (!Root->TryGetStringField(TEXT("type"), RootType) || !FindHjsonExternalInputFamilyContract(RootType))
			return RejectSource(TEXT("The root type does not identify a supported external-input family."),
				TEXT("/root/type"), Root->TryGetField(TEXT("type")), TEXT("supported external-input root type"));
		if (!Root->TryGetObjectField(TEXT("external_input"), External))
			return RejectSource(TEXT("The external-input body must be an object."), TEXT("/root/external_input"),
				Root->TryGetField(TEXT("external_input")), TEXT("object"));
		if (!(*External)->TryGetObjectField(TEXT("source"), Source))
			return RejectSource(TEXT("The external-input source must be an object with an identity."), TEXT("/root/external_input/source"),
				(*External)->TryGetField(TEXT("source")), TEXT("source object"));
		if (!(*Source)->TryGetStringField(TEXT("identity"), Identity))
			return RejectSource(TEXT("The external-input source identity must be a readable path string."), TEXT("/root/external_input/source/identity"),
				(*Source)->TryGetField(TEXT("identity")), TEXT("source path string"));
		if ((*Source)->HasField(TEXT("content_sha256")))
		{
			auto Detail = UE::UAI::Contract::FDiagnostic::Failure(TEXT("hjson_external_input_readable_source_required"),
				TEXT("The source content digest is managed internally and cannot be declared in editable input."), TEXT("external_import"));
			Detail.TargetPath = Target;
			Detail.Source.InternalJsonPath = TEXT("/root/external_input/source/content_sha256");
			Detail.ActualEvidence = (*Source)->TryGetField(TEXT("content_sha256"));
			OutError.SetFailure(MoveTemp(Detail));
			return false;
		}
		FString Path, Digest;
		if (!TryReadExternalInputSource(Identity, Path, Digest, OutError)) return false;
		if (!ExpectedSourceDigest.IsEmpty() && Digest != ExpectedSourceDigest)
		{
			auto Detail = UE::UAI::Contract::FDiagnostic::Failure(TEXT("hjson_external_input_source_digest_mismatch"),
				TEXT("The external source contents changed after preparation."), TEXT("external_import"));
			Detail.TargetPath = Target;
			Detail.ActualValue = MakeShared<FJsonValueString>(Identity);
			Detail.ActualEvidence = MakeShared<FJsonValueString>(Digest);
			Detail.ExpectedEvidence = MakeShared<FJsonValueString>(ExpectedSourceDigest);
			Detail.Source.InternalJsonPath = TEXT("/root/external_input/source/identity");
			OutError.SetFailure(MoveTemp(Detail));
			return false;
		}
		const TSharedRef<FJsonObject> Provenance = MakeShared<FJsonObject>();
		Provenance->SetStringField(TEXT("source"), Path);
		Provenance->SetStringField(TEXT("content_sha256"), Digest);
		FString ProvenanceText; UE::UAI::Contract::FDiagnostic JsonError;
		if (!Json::TryCanonicalize(Provenance, ProvenanceText, JsonError))
		{
			JsonError.SourceKind = TEXT("native_result");
			OutError.SetFailure(MoveTemp(JsonError));
			return false;
		}
		FUeAgentResourceTracker ResourceTracker = Resources();
		TMap<FString, FUeAgentCanonicalValueRef> Values;
		for (const auto& Field : (*External)->Values)
		{
			FUeAgentCanonicalValuePtr Value;
			if (!CanonicalJson::TryFromJsonValue(Field.Value, ResourceTracker, Value, JsonError))
			{
				OutError.SetFailure(JsonError, FString());
				return false;
			}
			Values.Add(Field.Key, Value.ToSharedRef());
		}
		TArray<FSemanticUnit> Units;
		Units.Emplace(TEXT("target"), ESemanticUnitKind::TargetState, FString(), RootType,
			TEXT("ExternalInput"), FString(), TMap<FString, FUeAgentCanonicalValueRef>{},
			TArray<FString>{}, TArray<FString>{}, FString(), FString(), TArray<FString>{},
			TArray<FString>{TEXT("llm_authoring")}, FString(), Target);
		Units.Emplace(TEXT("external"), ESemanticUnitKind::ExternalPayloadState, TEXT("target"),
			RootType, TEXT("Source"), FString(), MoveTemp(Values), TArray<FString>{},
			TArray<FString>{}, FString(), FString(), TArray<FString>{},
			TArray<FString>{TEXT("llm_authoring")}, ProvenanceText);
		return TryCompileAuthoringModel(GetExternalInputWorkspaceContract(), 1,
			TEXT("llm_authoring"), BaseRevision, MoveTemp(Units), {TEXT("target")},
			ResourceTracker, OutModel, OutError);
	}

	void FResolvedExternalInputDocument::LocateFailure(FWorkspaceError& Error, bool bDelegated) const
	{
		if (!Error.HasErrors()) return;
		auto Diagnostic = Error.GetDiagnostic();
		TFunction<void(Contract::FDiagnostic&)> Locate = [&](Contract::FDiagnostic& Issue)
		{
			for (auto& Child : Issue.Diagnostics) Locate(Child);
			if (Issue.PublicCause)
			{
				auto Cause = MakeShared<Contract::FDiagnostic>(*Issue.PublicCause);
				Locate(*Cause); Issue.PublicCause = MoveTemp(Cause);
			}
			if (!Issue.Source.SourceFile.IsEmpty() && Issue.Source.Line > 0
				&& !Issue.Source.SourceFile.StartsWith(TEXT("inline://"))
				&& !Issue.Source.SourceFile.StartsWith(TEXT("prepared:"))) return;
			FString Pointer = !Issue.SemanticPointer.IsEmpty() ? Issue.SemanticPointer
				: !Issue.Source.InternalJsonPath.IsEmpty() ? Issue.Source.InternalJsonPath : Issue.Source.JsonPath;
			const bool bHasField = !Pointer.IsEmpty() && Pointer != TEXT("/");
			bool bRelated = !bHasField || Issue.SourceKind == TEXT("related_input");
			if (!bHasField) Pointer = TEXT("/root/external_input");
			else if (!Pointer.StartsWith(TEXT("/root/")) && Pointer != TEXT("/root") && Pointer != TEXT("/schema"))
			{
				if (!Pointer.StartsWith(TEXT("/"))) Pointer = TEXT("/") + Pointer;
				if (bDelegated)
				{
					if (Pointer == TEXT("/source_ref") || Pointer == TEXT("/_hjson_expected_source_sha256"))
					{
						bRelated |= Pointer != TEXT("/source_ref");
						Pointer = TEXT("/root/external_input/source/identity");
					}
					else if (Pointer == TEXT("/_hjson_source_format")) Pointer = TEXT("/root/external_input/source/format");
					else if (Pointer == TEXT("/options") || Pointer.StartsWith(TEXT("/options/"))) Pointer = TEXT("/root/external_input") + Pointer;
					else Pointer = TEXT("/root/external_input/target_policy/parameters") + Pointer;
				}
				else Pointer = TEXT("/root") + Pointer;
			}
			if (Source.SemanticWorkspace)
			{
				FString UnitPointer = Pointer;
				if (UnitPointer.RemoveFromStart(TEXT("/root/external_input")))
				{
					Issue.Source.SourceUnit = TEXT("external");
					Issue.Source.InternalJsonPath = UnitPointer.IsEmpty() ? TEXT("/") : UnitPointer;
				}
				else
				{
					Issue.Source.SourceUnit = TEXT("target");
					Issue.Source.InternalJsonPath = TEXT("/");
					bRelated = true;
				}
				if (bRelated) Issue.SourceKind = TEXT("related_input");
				Source.SemanticWorkspace->LocateDocumentDiagnostic({}, {}, Issue);
				if (Issue.Source.SourceFile.IsEmpty() || Issue.Source.Line <= 0)
				{
					if (const auto* UnitSource = Source.SemanticWorkspace->GetUnitSources().Find(Issue.Source.SourceUnit))
					{
						Issue.Source.SourceFile = UnitSource->GetSourceUnit();
						Issue.Source.JsonPath = UnitSource->GetJsonPointer();
						Issue.Source.Line = UnitSource->GetLine(); Issue.Source.Column = UnitSource->GetColumn();
						Issue.SourceKind = TEXT("related_input");
					}
				}
			}
			else
			{
				bool bExact = false;
				const auto Location = Source.ResolveSource(Pointer, &bExact);
				if (!Location.SourceFile.IsEmpty())
				{
					Issue.Source = Location;
					Issue.SourceKind = bExact && !bRelated ? TEXT("authored_value") : TEXT("related_input");
				}
			}
		};
		Locate(Diagnostic);
		Error.SetFailure(MoveTemp(Diagnostic));
	}

	bool TryResolveExternalInputDocument(const FString& CanonicalParamsJson,
		const FString& RootType, FResolvedExternalInputDocument& OutDocument,
		FString& OutTarget, FWorkspaceError& OutError)
	{
		OutDocument = {};
		TSharedPtr<FJsonObject> Params;
		UE::UAI::Contract::FDiagnostic JsonError;
		if (!Json::TryParseObject(CanonicalParamsJson, Params, JsonError))
		{
			OutError.SetFailure(JsonError, FString());
			return false;
		}
		if (!Params->TryGetStringField(TEXT("target"), OutTarget) || OutTarget.IsEmpty())
		{
			OutError.SetFailure(Contract::FDiagnostic::Failure(TEXT("hjson_external_input_target_required"),
				TEXT("External-input import requires a non-empty target."), TEXT("input_decode")), FString(), TEXT("/target"));
			return false;
		}
		auto& Resolved = OutDocument.Source;
		if (!DomainAuthoring::TryDecodeResolvedDocument(OutTarget, CanonicalParamsJson,
			{TEXT("external_input"), TEXT("uai.hjson.external_input/1"), RootType}, Resolved, OutError))
			return false;
		ON_SCOPE_EXIT { OutDocument.LocateFailure(OutError); };
		auto Fail = [&](const FString& Code, const FString& Pointer, const FString& Reason)
		{
			auto Failure = Contract::FDiagnostic::Failure(Code, Reason, TEXT("input_decode"));
			Failure.SemanticPointer = Pointer;
			OutError.SetFailure(MoveTemp(Failure));
			return false;
		};
		auto LocateInputFailure = [&](const FString& Pointer)
		{
			auto Failure = OutError.GetDiagnostic();
			if (Failure.SemanticPointer.IsEmpty() && Failure.Source.JsonPath.IsEmpty()
				&& Failure.Source.InternalJsonPath.IsEmpty())
			{
				Failure.SemanticPointer = Pointer;
				Failure.SourceKind = TEXT("related_input");
			}
			OutError.SetFailure(MoveTemp(Failure));
			return false;
		};
		TSharedPtr<FJsonObject> Root = Resolved.Root;
		FString Digest;
		FString ExpectedTargetRevision;
		if (!Resolved.bSemanticWorkspaceDirectory
			&& (RootType == TEXT("uai.external_input.localization") || LoadObject<UObject>(nullptr, *OutTarget)))
		{
			return Fail(TEXT("hjson_external_input_existing_target_requires_workspace"), TEXT("/root"),
				FString::Printf(TEXT("Target '%s' requires an exported semantic workspace before import."), *OutTarget));
		}
		if (Resolved.bSemanticWorkspaceDirectory)
		{
			const auto& Model = Resolved.SemanticWorkspace->GetModel();
			ExpectedTargetRevision = Model->GetBaseRevision();
			const FSemanticUnit* External = Model->FindUnit(TEXT("external"));
			const FSemanticUnit* Target = Model->FindUnit(TEXT("target"));
			if (Model->GetContractId() != GetExternalInputWorkspaceContract()
				|| Model->GetUnits().Num() != 2 || !External || !Target
				|| Target->GetTargetIdentityKey() != OutTarget || Target->GetSemanticTypeRef() != RootType)
			{
				return Fail(TEXT("hjson_external_input_workspace_invalid"), TEXT("/root"),
					TEXT("The external-input workspace must contain its external and target units with matching target identity and root type."));
			}
			TSharedPtr<FJsonObject> Provenance;
			FString PreparedSource;
			if (!Json::TryParseObject(External->GetSourceProvenance(), Provenance, JsonError)
				|| !Provenance->TryGetStringField(TEXT("source"), PreparedSource)
				|| !Provenance->TryGetStringField(TEXT("content_sha256"), Digest))
			{
				return Fail(TEXT("hjson_external_input_source_baseline_missing"), TEXT("/root/external_input/source"),
					TEXT("The prepared source path or digest is missing from the workspace baseline; prepare the external source again."));
			}
			const TSharedRef<FJsonObject> Values = MakeShared<FJsonObject>();
			for (const auto& Field : External->GetValues())
			{
				TSharedPtr<FJsonValue> Value;
				if (!CanonicalJson::TryToJsonValue(Field.Value.Get(), Value, JsonError))
				{
					OutError.SetFailure(JsonError, FString());
					return LocateInputFailure(TEXT("/root/external_input/") + Field.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")));
				}
				Values->SetField(Field.Key, Value);
			}
			const TSharedPtr<FJsonObject>* Source = nullptr;
			FString Identity;
			if (!Values->TryGetObjectField(TEXT("source"), Source)
				|| !(*Source)->TryGetStringField(TEXT("identity"), Identity)
				|| (*Source)->HasField(TEXT("content_sha256")) || SourcePath(Identity) != PreparedSource)
			{
				return Fail(TEXT("hjson_external_input_source_requires_prepare"), TEXT("/root/external_input/source"),
					TEXT("Source identity must match the prepared source and must not author a content_sha256; prepare changed sources before importing."));
			}
			Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("type"), RootType);
			Root->SetObjectField(TEXT("external_input"), Values);
		}
		const TSharedPtr<FJsonObject>* External = nullptr;
		const TSharedPtr<FJsonObject>* Source = nullptr;
		FString Identity;
		if (!Root.IsValid() || !Root->TryGetObjectField(TEXT("external_input"), External)
			|| !(*External)->TryGetObjectField(TEXT("source"), Source)
			|| !(*Source)->TryGetStringField(TEXT("identity"), Identity)
			|| (*Source)->HasField(TEXT("content_sha256")))
		{
			return Fail(TEXT("hjson_external_input_readable_source_required"), TEXT("/root/external_input/source"),
				TEXT("External input requires source.identity as a file path and must not author a content_sha256."));
		}
		FString Path, CurrentDigest;
		if (!TryReadExternalInputSource(Identity, Path, CurrentDigest, OutError))
			return LocateInputFailure(TEXT("/root/external_input/source/identity"));
		if (!Digest.IsEmpty() && Digest != CurrentDigest)
		{
			return Fail(TEXT("hjson_external_input_source_digest_mismatch"), TEXT("/root/external_input/source/identity"),
				FString::Printf(TEXT("External source '%s' changed after preparation; prepare it again before importing."), *Identity));
		}
		if (!ExpectedTargetRevision.IsEmpty())
		{
			FString CurrentRevision;
			if (!TryReadExternalInputTargetRevision(OutTarget, CurrentRevision, OutError, RootType, Path))
				return LocateInputFailure(TEXT("/root/external_input/target_policy/parameters"));
			if (CurrentRevision != ExpectedTargetRevision)
			{
				return Fail(TEXT("hjson_external_input_target_changed"), TEXT("/root/external_input/target_policy/parameters"),
					FString::Printf(TEXT("Target '%s' changed after workspace export; export it again before importing."), *OutTarget));
			}
		}
		(*Source)->SetStringField(TEXT("identity"), Path);
		(*Source)->SetStringField(TEXT("content_sha256"), CurrentDigest);
		OutDocument.Root = MoveTemp(Root);
		return true;
	}
}

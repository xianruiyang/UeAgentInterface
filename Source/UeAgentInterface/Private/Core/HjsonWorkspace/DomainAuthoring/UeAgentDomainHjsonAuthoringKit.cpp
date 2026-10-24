#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"

#include "Algo/Unique.h"
#include "Composition/HjsonImport/UeAgentSemanticWorkspaceImport.h"
#include "Composition/HjsonImport/UeAgentPreviewWorkspaceApply.h"
#include "Core/AssetPreview/UeAgentAssetPreviewState.h"
#include "Containers/StringConv.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAssetLifecycle.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/HjsonWorkspace/UeAgentHjsonDiagnostic.h"
#include "Core/HjsonWorkspace/SourceLoader/UeAgentHjsonSourceLoader.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"
#include "Misc/Guid.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringDurableTransaction.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentNativeNormalizationIntent.h"
#include "Utilities/UeAgentFileOperationDiagnostic.h"
#include "Utilities/UeAgentJsonDiagnostics.h"

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END

namespace UE::UAI::HjsonWorkspace::DomainAuthoring
{
	using namespace ContextResource;
	using namespace DomainAdapters::ContentGraphRead;

	namespace
	{
		bool Fail(
			FWorkspaceError& OutError,
			UE::UAI::Contract::FDiagnostic Diagnostic,
			FString Source = FString(),
			FString Pointer = TEXT("/"))
		{
			OutError.SetFailure(MoveTemp(Diagnostic), Source, Pointer);
			return false;
		}

		Contract::FDiagnostic NativeFailure(const TCHAR* Code, const TCHAR* Reason, const FString& Path = FString())
		{
			auto Error = Contract::FDiagnostic::Failure(Code, Reason, TEXT("domain_authoring"));
			Error.PropertyPath = Path;
			Error.SourceKind = TEXT("native_result");
			return Error;
		}

		bool FailCaptureProjection(FWorkspaceError& OutError, Contract::FDiagnostic Error,
			const FString& Target, const FString& Prefix)
		{
			Error.SourceKind = TEXT("native_result");
			if (Error.TargetPath.IsEmpty()) Error.TargetPath = Target;
			const FString RelativePath = Error.Source.JsonPath == TEXT("/") ? FString() : Error.Source.JsonPath;
			Error.Source.JsonPath = Prefix + (RelativePath.IsEmpty() || RelativePath.StartsWith(TEXT("/"))
				? RelativePath : TEXT("/") + RelativePath);
			return Fail(OutError, MoveTemp(Error));
		}

		Contract::FDiagnostic ValidateDocumentContract(const FDomainHjsonDocumentContract& Value)
		{
			Contract::FDiagnostic Error;
			if (Value.DomainId.IsEmpty())
				Error = NativeFailure(TEXT("domain_hjson_contract_invalid"), TEXT("The domain document contract has an empty domain identifier."), TEXT("/contract/domain_id"));
			else if (Value.SchemaId.IsEmpty())
				Error = NativeFailure(TEXT("domain_hjson_contract_invalid"), TEXT("The domain document contract has an empty schema identifier."), TEXT("/contract/schema_id"));
			else if (Value.RootTypeIdentities.IsEmpty())
			{
				Error = NativeFailure(TEXT("domain_hjson_contract_invalid"), TEXT("The domain document contract declares no supported root types."), TEXT("/contract/root_types"));
				Error.ActualValue = MakeShared<FJsonValueNumber>(0);
			}
			return Error;
		}

		TSharedPtr<FJsonValue> RootTypeChoices(const TArray<FString>& Types)
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			for (const auto& Type : Types) Values.Add(MakeShared<FJsonValueString>(Type));
			return MakeShared<FJsonValueArray>(MoveTemp(Values));
		}

		bool AppendWarnings(
			const TArray<UE::UAI::Contract::FDiagnostic>& Notices,
			FString& Payload, FWorkspaceError& Error)
		{
			if (Notices.IsEmpty()) return true;
			TSharedPtr<FJsonObject> Result;
			UE::UAI::Contract::FDiagnostic JsonError;
		if (!CommandContract::Json::TryParseObject(Payload, Result, JsonError))
		{
			auto Failure = Contract::FDiagnostic::WrapFailure(TEXT("domain_hjson_readback_payload_invalid"), JsonError);
			Failure.SourceKind = TEXT("native_result");
			for (const auto& Notice : Notices) Failure.Append(Notice);
			return Fail(Error, MoveTemp(Failure));
		}
			TArray<TSharedPtr<FJsonValue>> Warnings;
			const TArray<TSharedPtr<FJsonValue>>* Existing = nullptr;
			if (Result->TryGetArrayField(TEXT("warnings"), Existing) && Existing) Warnings = *Existing;
			for (const UE::UAI::Contract::FDiagnostic& Notice : Notices) Warnings.Add(MakeShared<FJsonValueObject>(Notice.ToPublicJson()));
			Result->SetArrayField(TEXT("warnings"), Warnings);
			return TryCanonicalizeObject(Result, Payload, Error);
		}

		bool AppendRecoveryNotice(
            const UeAgentAuthoringWorkspaceTransaction::FScopedDurableRecoveryNotice& Notice,
            FString& Payload, FWorkspaceError& Error)
        {
            if (!Notice.HasPreservedMemoryEvidence()) return true;
            auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("durable_disk_restored_memory_preserved"),
                TEXT("The durable state was restored while the current in-memory evidence was preserved."), TEXT("durable_recovery"));
            Diagnostic.Severity = UE::UAI::Contract::ESeverity::Warning;
            return AppendWarnings({MoveTemp(Diagnostic)}, Payload, Error);
        }

		FWorkspaceError PreviewRestoreFailure(
			const FString& Target, const UE::UAI::Contract::FDiagnostic& NativeError)
		{
			auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(
				TEXT("preview_editor_state_restore_failed"),
				TEXT("Asset preview state could not be restored after editing."), TEXT("preview_restore"));
			Diagnostic.TargetPath = Target;
			Diagnostic.Append(NativeError);
			FWorkspaceError Error;
			Error.SetFailure(MoveTemp(Diagnostic), Target);
			return Error;
		}

		FString Sha256(const TArray<uint8>& Bytes)
		{
			uint8 Digest[SHA256_DIGEST_LENGTH] = {};
			const uint8 Empty = 0;
			SHA256(
				Bytes.IsEmpty() ? &Empty : Bytes.GetData(),
				static_cast<size_t>(Bytes.Num()),
				Digest);
			FString Result(TEXT("sha256:"));
			for (const uint8 Byte : Digest)
			{
				Result += FString::Printf(TEXT("%02x"), Byte);
			}
			return Result;
		}

		TArray<uint8> Utf8(const FString& Text)
		{
			const FTCHARToUTF8 Converted(*Text);
			TArray<uint8> Result;
			Result.Append(
				reinterpret_cast<const uint8*>(Converted.Get()),
				Converted.Length());
			return Result;
		}

		FString Sha256Text(const FString& Text)
		{
			return Sha256(Utf8(Text));
		}

		Contract::FDiagnostic ValidateEntryFile(const FString& EntryFile)
		{
			const TCHAR* Reason = EntryFile.IsEmpty() ? TEXT("The workspace entry filename is empty.")
				: !FPaths::IsRelative(EntryFile) || EntryFile.StartsWith(TEXT("/")) ? TEXT("The workspace entry filename must be relative.")
				: EntryFile.Contains(TEXT("\\")) || EntryFile.Contains(TEXT(":")) ? TEXT("The workspace entry filename must use forward slashes and cannot contain a colon.")
				: !EntryFile.EndsWith(TEXT(".hjson"), ESearchCase::IgnoreCase) ? TEXT("The workspace entry filename requires the .hjson extension.")
				: EntryFile.Equals(TEXT(".uai/workspace.hjson"), ESearchCase::IgnoreCase) ? TEXT("The workspace entry cannot use the reserved .uai/workspace.hjson manifest path.") : nullptr;
			if (!Reason)
			{
				TArray<FString> Segments;
				EntryFile.ParseIntoArray(Segments, TEXT("/"), false);
				if (Segments.IsEmpty()) Reason = TEXT("The workspace entry filename contains no path segment.");
				for (const FString& Segment : Segments)
				{
					if (Segment.IsEmpty() || Segment == TEXT(".") || Segment == TEXT(".."))
					{
						Reason = TEXT("The workspace entry filename cannot contain empty, dot or parent path segments.");
						break;
					}
				}
			}
			if (!Reason) return {};
			auto Error = NativeFailure(TEXT("domain_hjson_entry_file_invalid"), Reason, TEXT("/entry_file"));
			if (FPaths::IsRelative(EntryFile)) Error.ActualValue = MakeShared<FJsonValueString>(EntryFile);
			else Error.ActualEvidence = MakeShared<FJsonValueString>(EntryFile);
			return Error;
		}

		FUeAgentResourceTracker MakeResources()
		{
			return MakeSemanticWorkspaceResourceTracker();
		}

		bool AddFieldState(
			const FUeAgentContentGraphFieldState& State,
			const TSharedPtr<FJsonObject>& Value, UE::UAI::Contract::FDiagnostic& OutError)
		{
            if (State.Value.IsValid())
            {
                TSharedPtr<FJsonValue> StateValue;
                if (!StateSemantic::CanonicalJson::TryToJsonValue(*State.Value, StateValue, OutError)) return false;
                if (!StateValue.IsValid()) { OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("domain_hjson_state_value_unavailable"), TEXT("The captured state value could not be projected."), TEXT("domain_capture")); return false; }
                Value->SetField(TEXT("value"), StateValue);
            }
			const TCHAR* Override = TEXT("none");
			if (State.OverrideState == StateSemantic::EUeAgentOverrideState::Inherited)
			{
				Override = TEXT("inherited");
			}
			else if (State.OverrideState
				== StateSemantic::EUeAgentOverrideState::Overridden)
			{
				Override = TEXT("overridden");
			}
			Value->SetStringField(TEXT("override_state"), Override);
			Value->SetBoolField(
				TEXT("authored"),
				State.AuthoredPresence
					== StateSemantic::EUeAgentAuthoredPresence::Present);
            return true;
		}

		bool AddCanonicalState(
			const StateSemantic::FUeAgentCanonicalValuePtr& State,
			const TSharedPtr<FJsonObject>& Value, UE::UAI::Contract::FDiagnostic& OutError)
		{
            if (State.IsValid())
            {
                TSharedPtr<FJsonValue> StateValue;
                if (!StateSemantic::CanonicalJson::TryToJsonValue(*State, StateValue, OutError)) return false;
                if (!StateValue.IsValid()) { OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("domain_hjson_state_value_unavailable"), TEXT("The captured state value could not be projected."), TEXT("domain_capture")); return false; }
                Value->SetField(TEXT("value"), StateValue);
            }
            return true;
		}
	}

	FDomainHjsonDocumentContract::FDomainHjsonDocumentContract(
		FString InDomainId,
		FString InSchemaId,
		FString InRootTypeIdentity)
		: DomainId(MoveTemp(InDomainId))
		, SchemaId(MoveTemp(InSchemaId))
		, RootTypeIdentity(MoveTemp(InRootTypeIdentity))
	{
		RootTypeIdentities.Add(RootTypeIdentity);
	}

	FDomainHjsonDocumentContract::FDomainHjsonDocumentContract(
		FString InDomainId,
		FString InSchemaId,
		TArray<FString> InRootTypeIdentities)
		: DomainId(MoveTemp(InDomainId))
		, SchemaId(MoveTemp(InSchemaId))
		, RootTypeIdentities(MoveTemp(InRootTypeIdentities))
	{
		RootTypeIdentities.RemoveAll(
			[](const FString& Identity)
			{
				return Identity.TrimStartAndEnd().IsEmpty();
			});
		RootTypeIdentities.Sort();
		RootTypeIdentities.SetNum(
			Algo::Unique(RootTypeIdentities), EAllowShrinking::No);
		if (!RootTypeIdentities.IsEmpty())
		{
			RootTypeIdentity = RootTypeIdentities[0];
		}
	}

	bool FDomainHjsonDocumentContract::IsValid() const
	{
		return ValidateDocumentContract(*this).IsEmpty();
	}

	bool FDomainHjsonDocumentContract::SupportsRootType(
		const FString& InRootTypeIdentity) const
	{
		return RootTypeIdentities.Contains(InRootTypeIdentity);
	}

	bool FDomainHjsonImportLifecycleReceipt::IsComplete() const
	{
		return bMutationApplied
			&& bNativeUpdateSettled
			&& bPackageSaved;
	}

	bool FDomainHjsonCandidateSelector::IsValid() const
	{
		if (Name.TrimStartAndEnd().IsEmpty() || Select.Num() > 2)
		{
			return false;
		}
		for (const TPair<FString, FString>& Pair : Select)
		{
			if (Pair.Key.TrimStartAndEnd().IsEmpty()
				|| Pair.Value.TrimStartAndEnd().IsEmpty())
			{
				return false;
			}
		}
		return true;
	}

	bool FDomainHjsonCandidateSelector::TryGetSelectValue(
		const FString& Key,
		FString& OutValue) const
	{
		OutValue.Reset();
		for (const TPair<FString, FString>& Pair : Select)
		{
			if (Pair.Key.Equals(Key, ESearchCase::IgnoreCase))
			{
				OutValue = Pair.Value;
				return true;
			}
		}
		return false;
	}

	bool TryDecodeResolvedDocument(
		const FString& RequestTargetIdentity,
		const FString& CanonicalParamsJson,
		const FDomainHjsonDocumentContract& Contract,
		FResolvedDomainHjsonDocument& OutDocument,
		FWorkspaceError& OutError,
		EDomainHjsonDocumentProjection Projection)
	{
		OutDocument = {};
		OutError.Reset();
		if (const auto ContractError = ValidateDocumentContract(Contract); !ContractError.IsEmpty())
		{
			return Fail(OutError, ContractError);
		}
		TSharedPtr<FJsonObject> Params;
		UE::UAI::Contract::FDiagnostic JsonError;
		if (!CommandContract::Json::TryParseObject(
			CanonicalParamsJson, Params, JsonError) || !Params.IsValid())
		{
			return Fail(OutError, JsonError, FString(), TEXT("/"));
		}
		if (Params->HasField(TEXT("carrier"))
			|| Params->HasField(TEXT("hjson_document")))
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("domain_hjson_legacy_source_contract_rejected"),
				TEXT("The legacy carrier and hjson_document fields are unsupported; provide the document through source."), TEXT("domain_authoring"));
			Error.SourceKind = TEXT("command_input");
			Error.ActualValuePresent = true;
			return Fail(OutError, MoveTemp(Error), FString(), Params->HasField(TEXT("carrier")) ? TEXT("/carrier") : TEXT("/hjson_document"));
		}
		FString Target;
		FString ExpectedRevision;
		const TSharedPtr<FJsonObject>* Source = nullptr;
		if (!Params->TryGetStringField(TEXT("target"), Target) || Target.IsEmpty() || Target != RequestTargetIdentity)
		{
			auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("domain_hjson_target_or_revision_invalid"), TEXT("/target"),
				MakeShared<FJsonValueString>(RequestTargetIdentity), Params->TryGetField(TEXT("target")), TEXT("domain_authoring"),
				TEXT("The document import target must be a non-empty string identifying the requested target."));
			const auto ActualTarget = Params->TryGetField(TEXT("target"));
			if (ActualTarget.IsValid() && ActualTarget->Type != EJson::String)
			{
				Error.ActualEvidence = ActualTarget;
				Error.ActualValue = MakeShared<FJsonValueString>(UeAgentJsonDiagnostics::JsonValueTypeToString(ActualTarget));
			}
			Error.SourceKind = TEXT("command_input");
			return Fail(OutError, MoveTemp(Error), FString(), TEXT("/target"));
		}
		if (!Params->TryGetStringField(TEXT("expected_revision"), ExpectedRevision) || ExpectedRevision.IsEmpty())
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("domain_hjson_target_or_revision_invalid"),
				TEXT("The document import requires a non-empty expected revision string."), TEXT("domain_authoring"));
			Error.SourceKind = TEXT("command_input");
			Error.ActualValuePresent = Params->HasField(TEXT("expected_revision"));
			Error.ActualEvidence = Params->TryGetField(TEXT("expected_revision"));
			return Fail(OutError, MoveTemp(Error), FString(), TEXT("/expected_revision"));
		}
		if (!Params->TryGetObjectField(TEXT("source"), Source)
			|| !Source || !Source->IsValid())
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("domain_hjson_source_required"),
				TEXT("The document import requires a source object."), TEXT("domain_authoring"));
			Error.SourceKind = TEXT("command_input");
			Error.ActualValuePresent = Params->HasField(TEXT("source"));
			Error.ActualEvidence = Params->TryGetField(TEXT("source"));
			return Fail(OutError, MoveTemp(Error), FString(), TEXT("/source"));
		}
		FUeAgentResourceTracker Resources = MakeResources();
		FResolvedContentGraphImportSourcePtr ResolvedSource;
		if (!TryLoadNormalizedContentGraphImportSource(
				*Source,
				Target,
				ExpectedRevision,
				Resources,
				ResolvedSource,
				OutError)
			|| !ResolvedSource.IsValid())
		{
			return false;
		}
		if (ResolvedSource->IsSemanticWorkspaceDirectory())
		{
			FResolvedSemanticWorkspacePtr SemanticWorkspace;
			if (!TryLoadHjsonImportSource(
					FWorkspaceSource::Directory(
						ResolvedSource->GetSourceUnit()),
					ESemanticWorkspaceMode::State,
					Resources,
					SemanticWorkspace,
					OutError)
				|| !SemanticWorkspace.IsValid())
			{
				return false;
			}
			// Semantic workspaces own their revision in the machine manifest.
			// The public command's expected_revision can come from the legacy
			// content-graph read surface and is therefore not comparable with
			// the capture:* revision produced by authoring composition. The
			// semantic import coordinator independently recaptures the target
			// and validates this manifest revision before any mutation.
			const FString SemanticBaseRevision =
				SemanticWorkspace->GetModel()->GetBaseRevision();
			OutDocument.TargetIdentity = MoveTemp(Target);
			OutDocument.ExpectedRevision = SemanticBaseRevision;
			OutDocument.Carrier = TEXT("directory");
			OutDocument.SourceUnit = ResolvedSource->GetSourceUnit();
			OutDocument.BaseRevision = SemanticBaseRevision;
			OutDocument.Params = MoveTemp(Params);
			OutDocument.SemanticWorkspace = MoveTemp(SemanticWorkspace);
			OutDocument.bSemanticWorkspaceDirectory = true;
			return true;
		}
		const FContentGraphImportDocument* EntryDocument =
			ResolvedSource->GetEntryDocument();
		if (!EntryDocument)
		{
			auto Error = NativeFailure(TEXT("domain_hjson_entry_document_missing"), TEXT("The resolved import source does not contain its declared entry document."));
			Error.TargetPath = Target;
			Error.Source.SourceFile = ResolvedSource->GetEntryFile();
			Error.ActualValuePresent = false;
			return Fail(OutError, MoveTemp(Error), ResolvedSource->GetEntryFile());
		}
		FHjsonDocumentPtr Document;
		if (!TryParseHjsonDocument(
			EntryDocument->GetDocument(),
			ResolvedSource->GetSourceUnit(),
			Resources,
			Document,
			OutError)
			|| !Document.IsValid())
		{
			return false;
		}
		OutDocument.SourceUnit = ResolvedSource->GetSourceUnit();
		OutDocument.EntryFile = ResolvedSource->GetEntryFile();
		OutDocument.AstDocument = Document;
		OutDocument.AstRoot = &Document->GetRoot().Get();
		FScopedDomainHjsonDiagnosticSource DiagnosticSource(OutDocument, OutError);
		const FHjsonNode* SchemaNode = Document->GetRoot()->Get(TEXT("schema"));
		const FHjsonNode* RootNode = Document->GetRoot()->Get(TEXT("root"));
		const FHjsonNode* TypeNode = RootNode && RootNode->GetKind() == EHjsonNodeKind::Object
			? RootNode->Get(TEXT("type")) : nullptr;
		if (!SchemaNode || SchemaNode->GetKind() != EHjsonNodeKind::String)
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("domain_hjson_schema_missing"), TEXT("The document schema must be a string."), TEXT("domain_authoring"));
			Error.ExpectedValue = MakeShared<FJsonValueString>(Contract.SchemaId);
			Error.ActualValuePresent = SchemaNode != nullptr;
			Error.ActualValue = HjsonDiagnosticValue(SchemaNode);
			return Fail(OutError, MoveTemp(Error), FString(), TEXT("/schema"));
		}
		const FString& Schema = SchemaNode->GetText();
		if (Schema != Contract.SchemaId)
		{
			return Fail(OutError, UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("domain_hjson_schema_mismatch"), TEXT("/schema"),
				MakeShared<FJsonValueString>(Contract.SchemaId), MakeShared<FJsonValueString>(Schema), TEXT("domain_authoring")), FString(), TEXT("/schema"));
		}
		const bool bResolvedCandidate = !TypeNode && RootNode && RootNode->Get(TEXT("candidate"))
			&& Contract.RootTypeIdentities.Num() == 1;
		if (!bResolvedCandidate && (!TypeNode || TypeNode->GetKind() != EHjsonNodeKind::String))
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("domain_hjson_root_missing"),
				!RootNode ? TEXT("The document requires a root object.") : RootNode->GetKind() != EHjsonNodeKind::Object ? TEXT("The document root must be an object.") : TEXT("The document root requires a string type or a resolvable candidate."), TEXT("domain_authoring"));
			Error.ExpectedValue = !RootNode || RootNode->GetKind() != EHjsonNodeKind::Object
				? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(TEXT("object"))) : RootTypeChoices(Contract.RootTypeIdentities);
			Error.ActualValuePresent = RootNode && RootNode->GetKind() == EHjsonNodeKind::Object ? TypeNode != nullptr : RootNode != nullptr;
			Error.ActualValue = HjsonDiagnosticValue(RootNode && RootNode->GetKind() == EHjsonNodeKind::Object ? TypeNode : RootNode);
			return Fail(OutError, MoveTemp(Error), FString(), !RootNode || RootNode->GetKind() != EHjsonNodeKind::Object ? TEXT("/root") : TEXT("/root/type"));
		}
		const FString RootType = bResolvedCandidate ? Contract.RootTypeIdentity : TypeNode->GetText();
		if (!Contract.SupportsRootType(RootType))
		{
			return Fail(OutError, UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("domain_hjson_root_type_mismatch"), TEXT("/root/type"),
				RootTypeChoices(Contract.RootTypeIdentities), MakeShared<FJsonValueString>(RootType), TEXT("domain_authoring"), TEXT("The document root type is not supported by this domain contract.")), FString(), TEXT("/root/type"));
		}
		TSharedPtr<FJsonObject> JsonRoot;
		if (Projection == EDomainHjsonDocumentProjection::Json)
		{
			TSharedPtr<FJsonValue> Converted;
			if (!TryProjectHjsonNodeToJsonValue(*Document->GetRoot(), Resources, Converted, OutError)) return false;
			JsonRoot = Converted->AsObject()->GetObjectField(TEXT("root"));
		}
		OutDocument.TargetIdentity = MoveTemp(Target);
		OutDocument.ExpectedRevision = MoveTemp(ExpectedRevision);
		OutDocument.Carrier =
			ResolvedSource->GetKind() == EWorkspaceSourceKind::Directory
				? TEXT("directory")
				: ResolvedSource->GetKind() == EWorkspaceSourceKind::SingleFile
					? TEXT("single_file")
					: TEXT("inline");
		OutDocument.SourceUnit = ResolvedSource->GetSourceUnit();
		OutDocument.BaseRevision = ResolvedSource->GetBaseRevision();
		OutDocument.EntryFile = ResolvedSource->GetEntryFile();
		OutDocument.SourceFingerprint = ResolvedSource->GetSourceFingerprint();
		OutDocument.SemanticFingerprint =
			ResolvedSource->GetSemanticFingerprint();
		OutDocument.Params = MoveTemp(Params);
		OutDocument.Root = MoveTemp(JsonRoot);
		OutDocument.AstRoot = RootNode;
		OutDocument.AstDocument = MoveTemp(Document);
		return true;
	}

	namespace
	{
		Contract::FSourceLocation ResolveDomainDocumentSource(const FResolvedDomainHjsonDocument& Document,
			const FString& Requested, bool& bExact)
		{
			Contract::FSourceLocation Location;
			bExact = false;
			if (!Document.AstDocument || !Document.AstRoot) return Location;
			FString Pointer = Requested;
			if (Pointer.IsEmpty() || Pointer == TEXT("/")) Pointer = TEXT("/root");
			else if (Pointer != TEXT("/root") && !Pointer.StartsWith(TEXT("/root/"))
				&& Pointer != TEXT("/schema")) Pointer = TEXT("/root") + Pointer;
			const FString ExactPointer = Pointer;
			const FHjsonNode* Node = Document.AstDocument->Find(Pointer);
			while (!Node)
			{
				int32 Slash = INDEX_NONE;
				if (!Pointer.FindLastChar(TEXT('/'), Slash) || Slash == 0) break;
				Pointer.LeftInline(Slash);
				Node = Document.AstDocument->Find(Pointer);
			}
			if (!Node) Node = Document.AstRoot;
			const auto& Source = Node->GetProvenance();
			Location.SourceFile = Document.SourceUnit;
			Location.SourceUnit = Document.SourceUnit;
			Location.JsonPath = Source.GetJsonPointer();
			Location.InternalJsonPath = ExactPointer;
			Location.Line = Source.GetLine();
			Location.Column = Source.GetColumn();
			bExact = !Requested.IsEmpty() && Requested != TEXT("/") && Source.GetJsonPointer() == ExactPointer;
			return Location;
		}
	}

	FScopedDomainHjsonDiagnosticSource::FScopedDomainHjsonDiagnosticSource(
		const FResolvedDomainHjsonDocument& InDocument, FWorkspaceError& InError, FString InPointerPrefix)
		: Document(InDocument), Error(InError), PointerPrefix(MoveTemp(InPointerPrefix))
	{
		if (Document.Root && Document.AstDocument && PointerPrefix.IsEmpty())
			Locations = MakeUnique<UeAgentAuthoringDiagnostics::FScopedSourceLocations>(Document.Root.Get(),
				[&InDocument](const FString& Pointer)
				{
					bool bExact = false;
					return ResolveDomainDocumentSource(InDocument, TEXT("/root") + Pointer, bExact);
				});
	}

	Contract::FSourceLocation FResolvedDomainHjsonDocument::ResolveSource(const FString& Pointer, bool* bOutExact) const
	{
		bool bExact = false;
		auto Source = ResolveDomainDocumentSource(*this, Pointer, bExact);
		if (bOutExact) *bOutExact = bExact;
		return Source;
	}

	void FResolvedDomainHjsonDocument::LocateDiagnostic(Contract::FDiagnostic& Diagnostic, const FString& PointerPrefix) const
	{
		if (!Diagnostic.IsValid() || !AstDocument) return;
		TFunction<void(Contract::FDiagnostic&)> Locate = [&](Contract::FDiagnostic& Issue)
		{
			for (auto& Child : Issue.Diagnostics) Locate(Child);
			if (Issue.PublicCause)
			{
				auto Cause = MakeShared<Contract::FDiagnostic>(*Issue.PublicCause);
				Locate(*Cause); Issue.PublicCause = MoveTemp(Cause);
			}
			// Preserve already-bound original leaves. Generated inline locations and
			// file labels without parser coordinates still need the original AST.
			if (!Issue.Source.SourceFile.IsEmpty() && Issue.Source.Line > 0
				&& !Issue.Source.SourceFile.StartsWith(TEXT("resolved:"))
				&& (!Issue.Source.SourceFile.StartsWith(TEXT("inline://")) || Issue.Source.SourceFile == SourceUnit)) return;
			if (Issue.SourceKind == TEXT("command_input") || Issue.SourceKind == TEXT("native_result")) return;
			FString Pointer = !Issue.SemanticPointer.IsEmpty() ? Issue.SemanticPointer
				: !Issue.Source.InternalJsonPath.IsEmpty() ? Issue.Source.InternalJsonPath : Issue.Source.JsonPath;
			const bool bHasPointer = !Pointer.IsEmpty() && Pointer != TEXT("/");
			if (!PointerPrefix.IsEmpty() && Pointer != TEXT("/root") && !Pointer.StartsWith(TEXT("/root/")) && Pointer != TEXT("/schema"))
				Pointer = PointerPrefix + (bHasPointer ? Pointer : FString());
			bool bExact = false;
			const auto Source = ResolveDomainDocumentSource(*this, Pointer, bExact);
			if (!Source.SourceFile.IsEmpty())
			{
				Issue.Source = Source;
				Issue.SourceKind = bHasPointer && bExact && Issue.SourceKind != TEXT("related_input") ? TEXT("authored_value") : TEXT("related_input");
			}
		};
		Locate(Diagnostic);
	}

	FScopedDomainHjsonDiagnosticSource::~FScopedDomainHjsonDiagnosticSource()
	{
		if (!Error.HasErrors()) return;
		auto Diagnostic = Error.GetDiagnostic();
		Document.LocateDiagnostic(Diagnostic, PointerPrefix);
		Error.SetFailure(MoveTemp(Diagnostic));
	}

	FString ObjectPathFromPackageIdentity(const FString& TargetIdentity)
	{
		if (TargetIdentity.Contains(TEXT("."))) return TargetIdentity;
		const FString AssetName =
			FPackageName::GetLongPackageAssetName(TargetIdentity);
		return AssetName.IsEmpty()
			? TargetIdentity
			: TargetIdentity + TEXT(".") + AssetName;
	}

	bool TryInspectObjectTargetPresence(
		const FString& TargetIdentity,
		bool& bOutPresent)
	{
		bOutPresent = false;
		const FString PackageIdentity = TargetIdentity.Contains(TEXT("."))
			? FPackageName::ObjectPathToPackageName(TargetIdentity)
			: TargetIdentity;
		if (!FPackageName::IsValidLongPackageName(PackageIdentity))
		{
			return false;
		}

		bOutPresent = StaticFindObject(
			UObject::StaticClass(),
			nullptr,
			*ObjectPathFromPackageIdentity(TargetIdentity)) != nullptr
			|| FPackageName::DoesPackageExist(PackageIdentity);
		return true;
	}

	bool TryCaptureObject(
		const FString& TargetIdentity,
		const FString& ExpectedRootTypeIdentity,
		UObject*& OutObject,
		FUeAgentContentGraphCapture& OutCapture,
		FWorkspaceError& OutError)
	{
		return TryCaptureObject(
			TargetIdentity,
			TArray<FString>{ExpectedRootTypeIdentity},
			OutObject,
			OutCapture,
			OutError);
	}

	bool TryCaptureObject(
		const FString& TargetIdentity,
		const TArray<FString>& ExpectedRootTypeIdentities,
		UObject*& OutObject,
		FUeAgentContentGraphCapture& OutCapture,
		FWorkspaceError& OutError)
	{
		OutObject = StaticLoadObject(
			UObject::StaticClass(), nullptr,
			*ObjectPathFromPackageIdentity(TargetIdentity));
		if (!OutObject)
		{
			auto Error = NativeFailure(TEXT("domain_hjson_target_unresolved"), TEXT("The target object could not be loaded for capture."), TEXT("/target"));
			Error.TargetPath = TargetIdentity;
			return Fail(OutError, MoveTemp(Error));
		}
		if (!OutObject->GetClass()
			|| !ExpectedRootTypeIdentities.Contains(
				OutObject->GetClass()->GetPathName()))
		{
			auto Error = NativeFailure(TEXT("domain_hjson_target_type_mismatch"), TEXT("The target object class is not supported by this domain capture."), TEXT("/root/type"));
			Error.TargetPath = TargetIdentity;
			Error.ExpectedValue = RootTypeChoices(ExpectedRootTypeIdentities);
			Error.ActualValuePresent = OutObject->GetClass() != nullptr;
			if (OutObject->GetClass())
			{
				Error.ActualValue = MakeShared<FJsonValueString>(OutObject->GetClass()->GetName());
				Error.ActualEvidence = MakeShared<FJsonValueString>(OutObject->GetClass()->GetPathName());
			}
			return Fail(OutError, MoveTemp(Error));
		}
		FUeAgentContentGraphReadError CaptureError;
		if (!FUeAgentContentGraphReadProductionEnvironment::TryCaptureOwner(
			*OutObject, OutCapture, CaptureError))
		{
			if (CaptureError.IsEmpty()) CaptureError = NativeFailure(TEXT("domain_hjson_capture_failed"),
				TEXT("The native content graph capture failed without reporting a cause."));
			if (CaptureError.TargetPath.IsEmpty()) CaptureError.TargetPath = TargetIdentity;
			return Fail(OutError, MoveTemp(CaptureError));
		}
		return true;
	}

	bool TryBuildCaptureSnapshot(
		const FString& TargetIdentity,
		const FString& RootTypeIdentity,
		const FUeAgentContentGraphCapture& Capture,
		TSharedPtr<FJsonObject>& OutSnapshot,
		FWorkspaceError& OutError)
	{
		OutSnapshot.Reset();
		if (TargetIdentity.IsEmpty()
			|| RootTypeIdentity.IsEmpty()
			|| Capture.DomainId.IsEmpty()
			|| Capture.Revision.IsEmpty())
		{
			const TCHAR* Field = TargetIdentity.IsEmpty() ? TEXT("target") : RootTypeIdentity.IsEmpty() ? TEXT("root_type") : Capture.DomainId.IsEmpty() ? TEXT("domain_id") : TEXT("revision");
			auto Error = NativeFailure(TEXT("domain_hjson_capture_identity_invalid"),
				*FString::Printf(TEXT("The native capture has an empty %s field."), Field), FString(TEXT("/")) + Field);
			Error.TargetPath = TargetIdentity;
			Error.ActualValue = MakeShared<FJsonValueString>(FString());
			return Fail(OutError, MoveTemp(Error));
		}
		const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("target"), TargetIdentity);
		Result->SetStringField(TEXT("root_type"), RootTypeIdentity);
		Result->SetStringField(TEXT("domain_id"), Capture.DomainId);
		Result->SetStringField(TEXT("revision"), Capture.Revision);
		Result->SetBoolField(TEXT("complete"), true);
		Result->SetBoolField(TEXT("has_more"), false);

		TArray<TSharedPtr<FJsonValue>> Containers;
		for (const FUeAgentContentGraphContainer& Container : Capture.Containers)
		{
			const TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
			Value->SetStringField(TEXT("id"), Container.ContainerId);
			Value->SetStringField(TEXT("kind"), Container.ContainerKind);
			Value->SetStringField(TEXT("owner"), Container.OwnerId);
			Value->SetStringField(TEXT("parent"), Container.ParentContainerId);
			Value->SetNumberField(TEXT("order"), Container.Order);
			UE::UAI::Contract::FDiagnostic ConversionError;
            if (!AddCanonicalState(Container.State, Value, ConversionError)) {
                if (ConversionError.SubjectId.IsEmpty()) ConversionError.SubjectId = Container.ContainerId;
                return FailCaptureProjection(OutError, MoveTemp(ConversionError), TargetIdentity, FString::Printf(TEXT("/containers/%d/value"), Containers.Num()));
            }
			Containers.Add(MakeShared<FJsonValueObject>(Value));
		}
		Result->SetArrayField(TEXT("containers"), MoveTemp(Containers));

		TArray<TSharedPtr<FJsonValue>> Items;
		for (const FUeAgentContentGraphItem& Item : Capture.Items)
		{
			const TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
			Value->SetStringField(TEXT("id"), Item.ItemId);
			Value->SetStringField(TEXT("kind"), Item.ItemKind);
			Value->SetStringField(TEXT("class_path"), Item.ClassPath);
			Value->SetStringField(TEXT("container_id"), Item.ContainerId);
			Value->SetNumberField(TEXT("order"), Item.Order);
			UE::UAI::Contract::FDiagnostic ConversionError;
            if (!AddCanonicalState(Item.State, Value, ConversionError)) {
                if (ConversionError.SubjectId.IsEmpty()) ConversionError.SubjectId = Item.ItemId;
                return FailCaptureProjection(OutError, MoveTemp(ConversionError), TargetIdentity, FString::Printf(TEXT("/items/%d/value"), Items.Num()));
            }
			Items.Add(MakeShared<FJsonValueObject>(Value));
		}
		Result->SetArrayField(TEXT("items"), MoveTemp(Items));

		TArray<TSharedPtr<FJsonValue>> Relations;
		for (const FUeAgentContentGraphRelation& Relation : Capture.Relations)
		{
			const TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
			Value->SetStringField(TEXT("id"), Relation.RelationId);
			Value->SetStringField(TEXT("kind"), Relation.RelationKind);
			Value->SetStringField(TEXT("source"), Relation.SourceItemId);
			Value->SetStringField(TEXT("target"), Relation.TargetItemId);
			Relations.Add(MakeShared<FJsonValueObject>(Value));
		}
		Result->SetArrayField(TEXT("relations"), MoveTemp(Relations));

		TArray<TSharedPtr<FJsonValue>> Properties;
		for (const FUeAgentContentGraphProperty& Property : Capture.Properties)
		{
			const TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
			Value->SetStringField(TEXT("id"), Property.FieldId);
			Value->SetStringField(TEXT("owner"), Property.OwnerId);
			UE::UAI::Contract::FDiagnostic ConversionError;
            if (!AddFieldState(Property.State, Value, ConversionError)) {
                if (ConversionError.SubjectId.IsEmpty()) ConversionError.SubjectId = Property.FieldId;
                return FailCaptureProjection(OutError, MoveTemp(ConversionError), TargetIdentity, FString::Printf(TEXT("/properties/%d/value"), Properties.Num()));
            }
			Properties.Add(MakeShared<FJsonValueObject>(Value));
		}
		Result->SetArrayField(TEXT("properties"), MoveTemp(Properties));

		TArray<TSharedPtr<FJsonValue>> Parameters;
		for (const FUeAgentContentGraphParameter& Parameter : Capture.Parameters)
		{
			const TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
			Value->SetStringField(TEXT("id"), Parameter.ParameterId);
			Value->SetStringField(TEXT("owner"), Parameter.OwnerId);
			for (const TPair<FString, FString>& Selector
				: Parameter.SemanticIdentity.Selectors)
			{
				Value->SetStringField(Selector.Key, Selector.Value);
			}
			UE::UAI::Contract::FDiagnostic ConversionError;
            if (!AddFieldState(Parameter.State, Value, ConversionError)) {
                if (ConversionError.SubjectId.IsEmpty()) ConversionError.SubjectId = Parameter.ParameterId;
                return FailCaptureProjection(OutError, MoveTemp(ConversionError), TargetIdentity, FString::Printf(TEXT("/parameters/%d/value"), Parameters.Num()));
            }
			Parameters.Add(MakeShared<FJsonValueObject>(Value));
		}
		Result->SetArrayField(TEXT("parameters"), MoveTemp(Parameters));

		TArray<TSharedPtr<FJsonValue>> Diagnostics;
		for (const FUeAgentContentGraphDiagnostic& Diagnostic : Capture.Diagnostics)
		{
			const TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
			Value->SetStringField(TEXT("id"), Diagnostic.DiagnosticId);
			Value->SetStringField(TEXT("severity"), Diagnostic.Severity);
			Value->SetStringField(TEXT("code"), Diagnostic.Code);
			Value->SetStringField(TEXT("message"), Diagnostic.Message);
			Value->SetBoolField(TEXT("resolved"), Diagnostic.bResolved);
			Diagnostics.Add(MakeShared<FJsonValueObject>(Value));
		}
		Result->SetArrayField(TEXT("diagnostics"), MoveTemp(Diagnostics));
		if (Capture.DiagnosticCoverage.IsValid())
		{
			TSharedPtr<FJsonValue> Coverage;
			UE::UAI::Contract::FDiagnostic Detail;
			if (!StateSemantic::CanonicalJson::TryToJsonValue(*Capture.DiagnosticCoverage, Coverage, Detail))
				return FailCaptureProjection(OutError, MoveTemp(Detail), TargetIdentity, TEXT("/diagnostic_coverage"));
			Result->SetField(TEXT("diagnostic_coverage"), Coverage);
		}
		OutSnapshot = Result;
		OutError.Reset();
		return true;
	}

	bool TryCanonicalizeObject(
		const TSharedPtr<FJsonObject>& Object,
		FString& OutJson,
		FWorkspaceError& OutError)
	{
		UE::UAI::Contract::FDiagnostic Error;
		if (!Object.IsValid()
			|| !CommandContract::Json::TryCanonicalize(Object, OutJson, Error))
		{
			return Fail(OutError, Error.IsValid() ? Error : UE::UAI::Contract::FDiagnostic::Failure(
				TEXT("domain_hjson_json_invalid"), TEXT("The domain payload is not a JSON object."), TEXT("projection")));
		}
		return true;
	}

	static bool TryBuildDomainHjsonDirectoryExportPlanImpl(
		const FString& TargetIdentity,
		const FDomainHjsonDocumentContract& Contract,
		const FString& BaseRevision,
		const FString& ProfileId,
		const FString& ProviderId,
		const FString& EntryFile,
		const TSharedPtr<FJsonObject>& Document,
		bool bReadableEntry,
		FUeAgentResourceTracker& Resources,
		FExportWorkspacePlanPtr& OutPlan,
		FWorkspaceError& OutError);

	bool TryBuildDomainHjsonDirectoryExportPlan(
		const FString& TargetIdentity,
		const FDomainHjsonDocumentContract& Contract,
		const FString& BaseRevision,
		const FString& ProfileId,
		const FString& ProviderId,
		const FString& EntryFile,
		const TSharedPtr<FJsonObject>& Document,
		FUeAgentResourceTracker& Resources,
		FExportWorkspacePlanPtr& OutPlan,
		FWorkspaceError& OutError)
	{
		return TryBuildDomainHjsonDirectoryExportPlanImpl(
			TargetIdentity, Contract, BaseRevision, ProfileId, ProviderId,
			EntryFile, Document, false, Resources, OutPlan, OutError);
	}

	bool TryBuildReadableDomainHjsonDirectoryExportPlan(
		const FString& TargetIdentity,
		const FDomainHjsonDocumentContract& Contract,
		const FString& BaseRevision,
		const FString& ProfileId,
		const FString& ProviderId,
		const FString& EntryFile,
		const TSharedPtr<FJsonObject>& Document,
		FUeAgentResourceTracker& Resources,
		FExportWorkspacePlanPtr& OutPlan,
		FWorkspaceError& OutError)
	{
		return TryBuildDomainHjsonDirectoryExportPlanImpl(
			TargetIdentity, Contract, BaseRevision, ProfileId, ProviderId,
			EntryFile, Document, true, Resources, OutPlan, OutError);
	}

	static bool TryBuildDomainHjsonDirectoryExportPlanImpl(
		const FString& TargetIdentity,
		const FDomainHjsonDocumentContract& Contract,
		const FString& BaseRevision,
		const FString& ProfileId,
		const FString& ProviderId,
		const FString& EntryFile,
		const TSharedPtr<FJsonObject>& Document,
		const bool bReadableEntry,
		FUeAgentResourceTracker& Resources,
		FExportWorkspacePlanPtr& OutPlan,
		FWorkspaceError& OutError)
	{
		OutPlan.Reset();
		OutError.Reset();
		if (const auto ContractError = ValidateDocumentContract(Contract); !ContractError.IsEmpty())
			return Fail(OutError, UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("domain_hjson_export_identity_invalid"), ContractError));
		if (TargetIdentity.IsEmpty()
			|| BaseRevision.IsEmpty()
			|| ProfileId.IsEmpty()
			|| ProviderId.IsEmpty()
			|| !Document.IsValid())
		{
			const TCHAR* Field = TargetIdentity.IsEmpty() ? TEXT("target") : BaseRevision.IsEmpty() ? TEXT("base_revision") : ProfileId.IsEmpty() ? TEXT("profile_id") : ProviderId.IsEmpty() ? TEXT("provider_id") : TEXT("document");
			auto Error = NativeFailure(TEXT("domain_hjson_export_identity_invalid"),
				*FString::Printf(TEXT("The domain export requires a non-empty %s."), Field), FString(TEXT("/")) + Field);
			Error.TargetPath = TargetIdentity;
			if (TargetIdentity.IsEmpty() || BaseRevision.IsEmpty() || ProfileId.IsEmpty() || ProviderId.IsEmpty()) Error.ActualValue = MakeShared<FJsonValueString>(FString());
			else Error.ActualValuePresent = false;
			return Fail(OutError, MoveTemp(Error));
		}
		if (const auto EntryError = ValidateEntryFile(EntryFile); !EntryError.IsEmpty())
		{
			return Fail(OutError, UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("domain_hjson_export_entry_invalid"), EntryError));
		}
		FString Schema;
		const TSharedPtr<FJsonObject>* Root = nullptr;
		FString RootType;
		if (!Document->TryGetStringField(TEXT("schema"), Schema)
			|| Schema != Contract.SchemaId)
		{
			auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("domain_hjson_export_schema_mismatch"), TEXT("/schema"),
				MakeShared<FJsonValueString>(Contract.SchemaId), Document->TryGetField(TEXT("schema")), TEXT("domain_authoring"), TEXT("The export document schema must match the domain contract."));
			Error.SourceKind = TEXT("native_result");
			Error.TargetPath = TargetIdentity;
			return Fail(OutError, MoveTemp(Error));
		}
		if (!Document->TryGetObjectField(TEXT("root"), Root)
			|| !Root || !Root->IsValid()
			|| !(*Root)->TryGetStringField(TEXT("type"), RootType)
			|| !Contract.SupportsRootType(RootType))
		{
			auto Error = NativeFailure(TEXT("domain_hjson_export_root_type_mismatch"), TEXT("The export document requires an object root with a supported string type."), TEXT("/root/type"));
			Error.ExpectedValue = RootTypeChoices(Contract.RootTypeIdentities);
			Error.ActualValuePresent = Root && Root->IsValid() && (*Root)->HasField(TEXT("type"));
			if (!RootType.IsEmpty()) Error.ActualValue = MakeShared<FJsonValueString>(RootType);
			Error.TargetPath = TargetIdentity;
			return Fail(OutError, MoveTemp(Error));
		}

		FString CanonicalDocument;
		TArray<uint8> EntryBytes;
		if (bReadableEntry)
		{
			if (!TryCanonicalizeObject(Document, CanonicalDocument, OutError))
			{
				return false;
			}
			FHjsonDocumentPtr ParsedDocument;
			if (!TryParseHjsonDocument(
					CanonicalDocument,
					TEXT("domain-export:") + EntryFile,
					Resources,
					ParsedDocument,
					OutError)
				|| !ParsedDocument.IsValid()
				|| !TryWriteHjsonDocument(
					ParsedDocument.ToSharedRef(),
					Resources,
					EntryBytes,
					OutError))
			{
				return false;
			}
		}
		else if (!TryCanonicalizeObject(Document, CanonicalDocument, OutError))
		{
			return false;
		}
		if (!bReadableEntry)
		{
			EntryBytes = Utf8(CanonicalDocument);
		}
		const FString EntryHash = Sha256(EntryBytes);

		const TSharedRef<FJsonObject> Manifest = MakeShared<FJsonObject>();
		Manifest->SetStringField(
			TEXT("workspace_schema"), TEXT("uai.content_graph.workspace/1"));
		Manifest->SetNumberField(TEXT("format_version"), 1);
		Manifest->SetStringField(TEXT("target_identity"), TargetIdentity);
		Manifest->SetStringField(TEXT("base_revision"), BaseRevision);
		Manifest->SetStringField(TEXT("entry_file"), EntryFile);
		const TSharedRef<FJsonObject> File = MakeShared<FJsonObject>();
		File->SetStringField(TEXT("path"), EntryFile);
		File->SetStringField(TEXT("content_hash"), EntryHash);
		Manifest->SetArrayField(
			TEXT("files"), {MakeShared<FJsonValueObject>(File)});
		Manifest->SetArrayField(
			TEXT("references"), TArray<TSharedPtr<FJsonValue>>{});
		FString CanonicalManifest;
		if (!TryCanonicalizeObject(Manifest, CanonicalManifest, OutError))
		{
			return false;
		}
		TArray<uint8> ManifestBytes = Utf8(CanonicalManifest);
		const FString ManifestHash = Sha256(ManifestBytes);

		const FString IdentityMaterial = ProviderId + TEXT("|") + ProfileId
			+ TEXT("|") + TargetIdentity + TEXT("|") + BaseRevision
			+ TEXT("|") + Contract.DomainId + TEXT("|") + Contract.SchemaId
			+ TEXT("|") + RootType + TEXT("|") + EntryFile
			+ TEXT("|") + EntryHash + TEXT("|") + ManifestHash;
		const FString IdentityHash = Sha256Text(IdentityMaterial);
		TArray<FExportFilePlan> Files;
		Files.Emplace(EntryFile, MoveTemp(EntryBytes), EntryHash);
		Files.Emplace(
			TEXT(".uai/workspace.hjson"),
			MoveTemp(ManifestBytes),
			ManifestHash);
		Files.Sort(
			[](const FExportFilePlan& Left, const FExportFilePlan& Right)
			{
				return Left.GetRelativePath() < Right.GetRelativePath();
			});
		int64 OutputBytes = 0;
		for (const FExportFilePlan& PlannedFile : Files)
		{
			OutputBytes += PlannedFile.GetCanonicalUtf8().Num();
		}
		FUeAgentResourceChargeError ResourceError;
		if (!Resources.TryChargeMany(
			{
				{EUeAgentResourceDimension::OutputBytes, OutputBytes},
				{EUeAgentResourceDimension::StringBytes, OutputBytes},
				{EUeAgentResourceDimension::Files, Files.Num()},
				{EUeAgentResourceDimension::WorkUnits, Files.Num() + 1},
				{EUeAgentResourceDimension::EstimatedMemoryBytes, OutputBytes * 2},
			},
			TEXT("domain_hjson.export_plan"),
			ResourceError))
		{
			OutError.SetFailure(ResourceError);
			return false;
		}
		OutPlan = MakeShared<FExportWorkspacePlan>(
			EWorkspaceKind::Directory,
			EntryFile,
			TEXT("domain-hjson-snapshot:") + IdentityHash.Right(24),
			FUeAgentStateRevision(
				ProviderId,
				TEXT("domain-hjson-export:") + IdentityHash.Right(24),
				0),
			TEXT("domain-hjson-evidence:") + IdentityHash.Right(24),
			MoveTemp(Files),
			IdentityHash,
			(OutputBytes + 3) / 4);
		return true;
	}

	bool TryVerifyDomainHjsonStagedWorkspace(
		const FString& StagedWorkspacePath,
		const FString& TargetIdentity,
		const FDomainHjsonDocumentContract& Contract,
		const FString& BaseRevision,
		const FString& EntryFile,
		FUeAgentResourceTracker& Resources,
		FWorkspaceError& OutError)
	{
		OutError.Reset();
		if (const auto ContractError = ValidateDocumentContract(Contract); !ContractError.IsEmpty())
			return Fail(OutError, UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("domain_hjson_staging_identity_invalid"), ContractError));
		if (StagedWorkspacePath.IsEmpty()
			|| TargetIdentity.IsEmpty()
			|| BaseRevision.IsEmpty())
		{
			const TCHAR* Field = StagedWorkspacePath.IsEmpty() ? TEXT("staged_workspace") : TargetIdentity.IsEmpty() ? TEXT("target") : TEXT("base_revision");
			auto Error = NativeFailure(TEXT("domain_hjson_staging_identity_invalid"),
				*FString::Printf(TEXT("The staged workspace has a missing or invalid %s."), Field), FString(TEXT("/")) + Field);
			Error.TargetPath = TargetIdentity;
			Error.ActualValue = MakeShared<FJsonValueString>(FString());
			return Fail(OutError, MoveTemp(Error));
		}
		if (const auto EntryError = ValidateEntryFile(EntryFile); !EntryError.IsEmpty())
			return Fail(OutError, UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("domain_hjson_staging_identity_invalid"), EntryError));
		const TSharedRef<FJsonObject> Source = MakeShared<FJsonObject>();
		Source->SetStringField(TEXT("kind"), TEXT("directory"));
		Source->SetStringField(
			TEXT("path"), FPaths::ConvertRelativePathToFull(StagedWorkspacePath));
		FResolvedContentGraphImportSourcePtr Resolved;
		if (!TryLoadContentGraphImportSource(
			Source, TargetIdentity, Resources, Resolved, OutError)
			|| !Resolved.IsValid())
		{
			return false;
		}
		if (Resolved->GetKind() != EWorkspaceSourceKind::Directory
			|| Resolved->GetTargetIdentity() != TargetIdentity
			|| Resolved->GetBaseRevision() != BaseRevision
			|| Resolved->GetEntryFile() != EntryFile)
		{
			const bool bKindMismatch = Resolved->GetKind() != EWorkspaceSourceKind::Directory;
			const bool bTargetMismatch = Resolved->GetTargetIdentity() != TargetIdentity;
			const TCHAR* Field = bKindMismatch ? TEXT("source_kind") : bTargetMismatch ? TEXT("target") : Resolved->GetBaseRevision() != BaseRevision ? TEXT("base_revision") : TEXT("entry_file");
			auto Error = NativeFailure(TEXT("domain_hjson_staging_identity_mismatch"),
				*FString::Printf(TEXT("The staged workspace %s does not match the export plan."), Field), FString(TEXT("/")) + Field);
			Error.TargetPath = TargetIdentity;
			if (bKindMismatch)
			{
				Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("directory"));
				Error.ActualValue = MakeShared<FJsonValueString>(Resolved->GetKind() == EWorkspaceSourceKind::SingleFile ? TEXT("single_file") : TEXT("inline"));
			}
			else if (bTargetMismatch)
			{
				Error.ExpectedValue = MakeShared<FJsonValueString>(TargetIdentity);
				Error.ActualEvidence = MakeShared<FJsonValueString>(Resolved->GetTargetIdentity());
			}
			else if (Resolved->GetBaseRevision() != BaseRevision)
			{
				Error.ExpectedEvidence = MakeShared<FJsonValueString>(BaseRevision);
				Error.ActualEvidence = MakeShared<FJsonValueString>(Resolved->GetBaseRevision());
			}
			else if (Resolved->GetEntryFile() != EntryFile)
			{
				Error.ExpectedValue = MakeShared<FJsonValueString>(EntryFile);
				Error.ActualValue = MakeShared<FJsonValueString>(Resolved->GetEntryFile());
			}
			return Fail(OutError, MoveTemp(Error));
		}
		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target"), TargetIdentity);
		Params->SetStringField(TEXT("expected_revision"), BaseRevision);
		Params->SetObjectField(TEXT("source"), Resolved->MakeNormalizedSourceObject());
		FString CanonicalParams;
		if (!TryCanonicalizeObject(Params, CanonicalParams, OutError))
		{
			return false;
		}
		FResolvedDomainHjsonDocument Decoded;
		if (!TryDecodeResolvedDocument(
			TargetIdentity, CanonicalParams, Contract, Decoded, OutError))
		{
			return false;
		}
		if (Decoded.BaseRevision != BaseRevision
			|| Decoded.EntryFile != EntryFile)
		{
			const bool bRevisionMismatch = Decoded.BaseRevision != BaseRevision;
			auto Error = NativeFailure(TEXT("domain_hjson_staging_identity_mismatch"),
				bRevisionMismatch ? TEXT("The decoded staged workspace base revision does not match the export plan.") : TEXT("The decoded staged workspace entry file does not match the export plan."),
				bRevisionMismatch ? TEXT("/base_revision") : TEXT("/entry_file"));
			Error.TargetPath = TargetIdentity;
			if (bRevisionMismatch)
			{
				Error.ExpectedEvidence = MakeShared<FJsonValueString>(BaseRevision);
				Error.ActualEvidence = MakeShared<FJsonValueString>(Decoded.BaseRevision);
			}
			else
			{
				Error.ExpectedValue = MakeShared<FJsonValueString>(EntryFile);
				Error.ActualValue = MakeShared<FJsonValueString>(Decoded.EntryFile);
			}
			return Fail(OutError, MoveTemp(Error));
		}
		return true;
	}

	void SetWorkspaceErrorFromCommandFailure(
		const UE::UAI::Contract::FDiagnostic& Failure,
		FString FallbackCode,
		FString FallbackSubject,
		FWorkspaceError& OutError)
	{
		auto Diagnostic = Failure;
		if (Diagnostic.IsEmpty()) Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(
			MoveTemp(FallbackCode), TEXT("The domain command failed without a diagnostic."), TEXT("domain_execute"));
		OutError.SetFailure(MoveTemp(Diagnostic), FallbackSubject);
	}

	bool TryReadNativeMutationPayload(const FString& Target, const FString& Payload,
		TSharedPtr<FJsonObject>& OutMutation, FWorkspaceError& OutError, FDomainHjsonImportLifecycleReceipt* OutReceipt)
	{
		Contract::FDiagnostic ParseError;
		if (!Contract::Json::TryParseObject(Payload, OutMutation, ParseError) || !OutMutation)
		{
			if (ParseError.IsEmpty()) ParseError = Contract::FDiagnostic::Failure(TEXT("domain_hjson_native_result_invalid"),
				TEXT("The native command did not return an object."), TEXT("response_decode"));
			ParseError.SourceKind = TEXT("native_result");
			ParseError.TargetPath = Target;
			OutError.SetFailure(MoveTemp(ParseError));
			return false;
		}
		auto Invalid = [&](const TCHAR* Field, TSharedPtr<FJsonValue> Expected, const TCHAR* Reason)
		{
			auto Diagnostic = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("domain_hjson_native_result_invalid"),
				FString(TEXT("/")) + Field, Expected, OutMutation->TryGetField(Field), TEXT("response_decode"), Reason);
			Diagnostic.SourceKind = TEXT("native_result");
			Diagnostic.TargetPath = Target;
			OutError.SetFailure(MoveTemp(Diagnostic));
			return false;
		};
		FString MutationTarget;
		if (!OutMutation->TryGetStringField(TEXT("target_ref"), MutationTarget)
			|| ObjectPathFromPackageIdentity(MutationTarget) != ObjectPathFromPackageIdentity(Target))
			return Invalid(TEXT("target_ref"), MakeShared<FJsonValueString>(Target), TEXT("The native result must identify the requested target."));
		if (OutReceipt)
		{
			FString Status;
			if (!OutMutation->TryGetStringField(TEXT("terminal_status"), Status) || Status != TEXT("succeeded"))
				return Invalid(TEXT("terminal_status"), MakeShared<FJsonValueString>(TEXT("succeeded")), TEXT("The native lifecycle did not report successful completion."));
			for (const TCHAR* Field : {TEXT("mutation_applied"), TEXT("compile_settled"), TEXT("saved")})
			{
				bool bCompleted = false;
				if (!OutMutation->TryGetBoolField(Field, bCompleted) || !bCompleted)
					return Invalid(Field, MakeShared<FJsonValueBoolean>(true), TEXT("The native result did not confirm this required lifecycle stage."));
			}
			OutReceipt->bMutationApplied = true;
			OutReceipt->bNativeUpdateSettled = true;
			OutReceipt->bPackageSaved = true;
		}
		return true;
	}

	bool TryBuildImportSuccessPayload(
		const FString& ProviderId,
		const FString& TargetIdentity,
		const FString& Revision,
		const FDomainHjsonImportLifecycleReceipt& LifecycleReceipt,
		const TSharedPtr<FJsonObject>& IndependentReadback,
		const TSharedPtr<FJsonObject>& Mutation,
		FString& OutPayloadJson,
		FWorkspaceError& OutError)
	{
		if (ProviderId.IsEmpty()
			|| TargetIdentity.IsEmpty()
			|| Revision.IsEmpty()
			|| !IndependentReadback.IsValid())
		{
			const TCHAR* Field = ProviderId.IsEmpty() ? TEXT("provider_id") : TargetIdentity.IsEmpty() ? TEXT("target") : Revision.IsEmpty() ? TEXT("revision") : TEXT("independent_readback");
			auto Error = NativeFailure(TEXT("domain_hjson_result_identity_invalid"),
				*FString::Printf(TEXT("The import result requires a non-empty %s."), Field), FString(TEXT("/")) + Field);
			Error.TargetPath = TargetIdentity;
			if (ProviderId.IsEmpty() || TargetIdentity.IsEmpty() || Revision.IsEmpty()) Error.ActualValue = MakeShared<FJsonValueString>(FString());
			else Error.ActualValuePresent = false;
			return Fail(OutError, MoveTemp(Error));
		}
		if (!LifecycleReceipt.IsComplete())
		{
			const TCHAR* Field = !LifecycleReceipt.bMutationApplied ? TEXT("mutation_applied") : !LifecycleReceipt.bNativeUpdateSettled ? TEXT("native_update_settled") : TEXT("package_saved");
			auto Error = NativeFailure(TEXT("domain_hjson_lifecycle_receipt_incomplete"),
				*FString::Printf(TEXT("The import lifecycle receipt does not confirm %s."), Field), FString(TEXT("/lifecycle/")) + Field);
			Error.TargetPath = TargetIdentity;
			Error.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			Error.ActualValue = MakeShared<FJsonValueBoolean>(false);
			if (LifecycleReceipt.bMutationApplied) Error.MutationStarted = true;
			return Fail(OutError, MoveTemp(Error));
		}
		(void)Mutation;
		const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("status"), TEXT("succeeded"));
		return TryCanonicalizeObject(Result, OutPayloadJson, OutError);
	}

	bool TryReadbackImportPayload(
		const FString& ProviderId,
		const FString& TargetIdentity,
		const FString& ObjectPath,
		const TSharedPtr<FJsonObject>& Mutation,
		const FString& ReadbackPayloadErrorCode,
		const FString& CompileNotSettledErrorCode,
		TFunctionRef<bool(FString&, FString&, FWorkspaceError&)> TryReadback,
		FString& OutPayloadJson,
		FWorkspaceError& OutError,
		const UeAgentAuthoringWorkspaceTransaction::FFinalReadbackEvidence* CompletedOperation)
	{
		FString ReadbackPayload;
		FString Revision;
		if (!TryReadback(ReadbackPayload, Revision, OutError))
		{
			return false;
		}
		TSharedPtr<FJsonObject> Readback;
		UE::UAI::Contract::FDiagnostic JsonError;
		if (!CommandContract::Json::TryParseObject(
				ReadbackPayload, Readback, JsonError))
		{
			auto Diagnostic = UE::UAI::Contract::FDiagnostic::WrapFailure(ReadbackPayloadErrorCode, JsonError, TEXT("final_readback"));
			Diagnostic.SourceKind = TEXT("native_result");
			Diagnostic.TargetPath = TargetIdentity;
			OutError.SetFailure(MoveTemp(Diagnostic));
			return false;
		}
		if (!TryValidateReadbackDiagnostics(Readback, TargetIdentity, CompileNotSettledErrorCode, OutError)) return false;
		UeAgentAuthoringWorkspaceTransaction::FFinalReadbackEvidence Evidence;
		if (CompletedOperation) Evidence = *CompletedOperation;
		else if (!UeAgentAuthoringWorkspaceTransaction::TryGetFinalReadbackEvidence(Evidence))
		{
			auto Error = NativeFailure(TEXT("domain_hjson_readback_lifecycle_evidence_missing"), TEXT("Final readback requires the executing transaction's stage evidence; a snapshot is not an operation receipt."), TEXT("/lifecycle"));
			Error.TargetPath = TargetIdentity;
			Error.ActualValuePresent = false;
			return Fail(OutError, MoveTemp(Error));
		}

		FDomainHjsonImportLifecycleReceipt LifecycleReceipt;
		LifecycleReceipt.bMutationApplied = Evidence.bMutationPhaseCompleted;
		LifecycleReceipt.bNativeUpdateSettled = !Evidence.bNativeUpdateRequested || Evidence.bNativeUpdateCompleted;
		LifecycleReceipt.bPackageSaved = Evidence.bSaveCompleted;
		LifecycleReceipt.PersistenceReceiptId =
			TEXT("reload:") + ObjectPath + TEXT(":") + Revision;
		return TryBuildImportSuccessPayload(
			ProviderId,
			TargetIdentity,
			Revision,
			LifecycleReceipt,
			Readback,
			Mutation,
			OutPayloadJson,
			OutError);
	}

	bool TryValidateReadbackDiagnostics(
		const TSharedPtr<FJsonObject>& Readback,
		const FString& TargetIdentity,
		const FString& FailureCode,
		FWorkspaceError& OutError)
	{
		if (!Readback.IsValid())
		{
			auto Error = NativeFailure(*FailureCode, TEXT("Readback payload is missing."));
			Error.TargetPath = TargetIdentity;
			Error.ActualValuePresent = false;
			return Fail(OutError, MoveTemp(Error));
		}
		const TArray<TSharedPtr<FJsonValue>>* Diagnostics = nullptr;
		if (Readback->TryGetArrayField(TEXT("diagnostics"), Diagnostics) && Diagnostics)
		{
			for (int32 Index = 0; Index < Diagnostics->Num(); ++Index)
			{
				const auto Item = (*Diagnostics)[Index].IsValid() ? (*Diagnostics)[Index]->AsObject() : nullptr;
				if (!Item.IsValid()) continue;
				FString Severity, Message, NativeCode;
				bool bResolved = false;
				Item->TryGetStringField(TEXT("severity"), Severity);
				Item->TryGetBoolField(TEXT("resolved"), bResolved);
				if (bResolved || !Severity.Equals(TEXT("error"), ESearchCase::IgnoreCase)) continue;
				Item->TryGetStringField(TEXT("message"), Message);
				Item->TryGetStringField(TEXT("code"), NativeCode);
				auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(
					NativeCode.IsEmpty() ? FailureCode : NativeCode, Message.IsEmpty() ? TEXT("The native readback contains an unresolved error.") : Message, TEXT("final_readback"));
				Diagnostic.Source.JsonPath = FString::Printf(TEXT("/diagnostics/%d"), Index);
				Diagnostic.SourceKind = TEXT("native_result");
				Diagnostic.TargetPath = TargetIdentity;
				if (Diagnostic.Code != FailureCode) Diagnostic = UE::UAI::Contract::FDiagnostic::WrapFailure(FailureCode, Diagnostic, TEXT("final_readback"));
				return Fail(OutError, MoveTemp(Diagnostic), TargetIdentity);
			}
		}
		// Absence of messages is not a claim of compilation, execution or freshness.
		// Those operation facts are supplied by the native update phase/receipt.
		return true;
	}

	void AddStoredBlueprintDiagnostics(const UBlueprint& Blueprint, const TSharedRef<FJsonObject>& Readback)
	{
		const auto State = MakeShared<FJsonObject>();
		State->SetStringField(TEXT("owner"), Blueprint.GetPathName());
		State->SetStringField(TEXT("context"), TEXT("stored_blueprint"));
		State->SetStringField(TEXT("status"), UeAgentBlueprintAssetLifecycle::GetStoredCompileObservationStatus(Blueprint));
		Readback->SetObjectField(TEXT("native_compile_state"), State);
		TArray<TSharedPtr<FJsonValue>> Diagnostics;
		const TArray<TSharedPtr<FJsonValue>>* Existing = nullptr;
		if (Readback->TryGetArrayField(TEXT("diagnostics"), Existing) && Existing) Diagnostics = *Existing;
		if (Blueprint.Status == BS_Error || Blueprint.Status == BS_UpToDateWithWarnings)
		{
			const auto Message = MakeShared<FJsonObject>();
			Message->SetStringField(TEXT("severity"), Blueprint.Status == BS_Error ? TEXT("error") : TEXT("warning"));
			Message->SetStringField(TEXT("code"), TEXT("blueprint_compile_status"));
			Message->SetStringField(TEXT("message"), Blueprint.Status == BS_Error
				? TEXT("The last Blueprint compilation failed.") : TEXT("The last Blueprint compilation reported warnings."));
			Diagnostics.Add(MakeShared<FJsonValueObject>(Message));
		}
		Readback->SetArrayField(TEXT("diagnostics"), MoveTemp(Diagnostics));
	}

	bool TryReadPersistedPackageRevision(
		const FString& PackageIdentity,
		FString& OutRevision,
		FWorkspaceError& OutError)
	{
		OutRevision.Reset();
		const FString PackageName =
			FPackageName::ObjectPathToPackageName(PackageIdentity);
		if (!FPackageName::IsValidLongPackageName(PackageName))
		{
			auto Error = NativeFailure(TEXT("domain_hjson_persisted_package_identity_invalid"), TEXT("The target does not identify a valid Unreal package."), TEXT("/target"));
			Error.TargetPath = PackageIdentity;
			Error.ActualEvidence = MakeShared<FJsonValueString>(PackageName);
			return Fail(OutError, MoveTemp(Error));
		}
		const FString Filename = FPackageName::LongPackageNameToFilename(
			PackageName, FPackageName::GetAssetPackageExtension());
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *Filename))
		{
			auto Error = UE::UAI::FileDiagnostics::MakeFileOperationFailure(TEXT("domain_hjson_persisted_package_missing"),
				TEXT("The persisted target package could not be read."), TEXT("domain_authoring"), Filename);
			Error.TargetPath = PackageIdentity;
			return Fail(OutError, MoveTemp(Error));
		}
		if (Bytes.IsEmpty())
		{
			auto Error = NativeFailure(TEXT("domain_hjson_persisted_package_missing"), TEXT("The persisted target package file is empty."));
			Error.TargetPath = PackageIdentity;
			Error.ActualValue = MakeShared<FJsonValueNumber>(0);
			Error.InternalContext = Filename;
			return Fail(OutError, MoveTemp(Error));
		}
		OutRevision = Sha256(Bytes);
		return true;
	}

	bool TryCoordinateRootPropertyImport(
		const FResolvedSemanticWorkspaceRef& Workspace,
		const FAuthoringModelRef& CurrentModel,
		const StateSemantic::FUeAgentCanonicalSnapshotRef& CurrentSnapshot,
		Contract::FUeAgentDescriptorIdentity DescriptorIdentity,
		const FString& ExpectedRootOwnerPath,
		FUeAgentResourceTracker& Resources,
		const FRootPropertyImportContract& ImportContract,
		TFunctionRef<bool(const FString&, TArray<TSharedPtr<FJsonValue>>&&,
			FRootPropertyApplyOutcome&)> TryApply,
		TFunctionRef<bool(FString&, FString&, FWorkspaceError&)> TryReadback,
		FString& OutPayloadJson,
		FWorkspaceError& OutError)
	{
		using namespace Composition::HjsonImport;

		const FString RequestId = ImportContract.RequestPrefix + TEXT("-import-")
			+ FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
		const FString IdempotencyKey = ImportContract.RequestPrefix + TEXT(":")
			+ Workspace->GetWorkspaceFingerprint();
		const Contract::FUeAgentCallerContext Caller(
			ImportContract.RequestPrefix + TEXT("-import"),
			{TEXT("uai:query"), TEXT("uai:operate"), TEXT("authoring.write")},
			RequestId);
		TArray<TSharedPtr<FJsonValue>> Properties;
		FSemanticWorkspaceImportError ImportError;
		if (!TryPrepareRootObjectPropertyChanges(
				Workspace, CurrentModel, CurrentSnapshot, MoveTemp(DescriptorIdentity),
				RequestId, IdempotencyKey, Caller, ExpectedRootOwnerPath, Resources,
				Properties, ImportError))
		{
			return Fail(OutError, ImportError.IsValid() ? ImportError
				: UE::UAI::Contract::FDiagnostic::Failure(ImportContract.PrepareFailedErrorCode,
					TEXT("The import could not be prepared."), TEXT("preflight")), ImportContract.TargetIdentity);
		}

		UeAgentAuthoringWorkspaceTransaction::FScopedDurableRecoveryNotice RecoveryNotice;
		FRootPropertyApplyOutcome ApplyOutcome;
		bool bFinalReadbackCompleted = false;
		ApplyOutcome.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure)
		{
			FWorkspaceError ReadError;
			bFinalReadbackCompleted = TryReadbackImportPayload(
				ImportContract.ProviderId, ImportContract.TargetIdentity, ImportContract.ObjectPath,
				nullptr, ImportContract.ReadbackPayloadErrorCode,
				ImportContract.CompileNotSettledErrorCode, TryReadback, OutPayloadJson, ReadError);
			if (bFinalReadbackCompleted)
				bFinalReadbackCompleted = AppendRecoveryNotice(RecoveryNotice, OutPayloadJson, ReadError);
			if (!bFinalReadbackCompleted)
			{
				Failure.Diagnostic = ReadError.GetDiagnostic();
			}
			return bFinalReadbackCompleted;
		};
		if (!TryApply(RequestId, MoveTemp(Properties), ApplyOutcome)
			|| !ApplyOutcome.bApplied)
		{
			Contract::FDiagnostic Diagnostic = ApplyOutcome.Diagnostic;
			if (Diagnostic.IsEmpty()) Diagnostic = Contract::FDiagnostic::Failure(
				ImportContract.ApplyFailedErrorCode, TEXT("The asset operation failed without a diagnostic."), ApplyOutcome.Phase);
			if (Diagnostic.Stage.IsEmpty()) Diagnostic.Stage = ApplyOutcome.Phase;
			if (Diagnostic.TargetPath.IsEmpty()) Diagnostic.TargetPath = ImportContract.TargetIdentity;
			OutError.SetFailure(MoveTemp(Diagnostic));
			return false;
		}
		if (!bFinalReadbackCompleted)
		{
			auto Error = NativeFailure(TEXT("domain_hjson_atomic_readback_not_executed"), TEXT("The asset operation reported applied without completing its required final readback."), TEXT("/final_readback"));
			Error.Stage = ApplyOutcome.Phase;
			Error.TargetPath = ImportContract.TargetIdentity;
			Error.MutationStarted = true;
			Error.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			Error.ActualValue = MakeShared<FJsonValueBoolean>(false);
			return Fail(OutError, MoveTemp(Error));
		}
		return true;
	}

	bool TryCoordinateSemanticWorkspaceImport(
		const FResolvedSemanticWorkspaceRef& Workspace,
		const FAuthoringModelRef& CurrentModel,
		const StateSemantic::FUeAgentCanonicalSnapshotRef& CurrentSnapshot,
		Contract::FUeAgentDescriptorIdentity DescriptorIdentity,
		const FString& ExpectedRootOwnerPath,
		FUeAgentResourceTracker& Resources,
		const FRootPropertyImportContract& ImportContract,
		TFunctionRef<bool(
			const FString&,
			const Composition::HjsonImport::FPreparedAuthoringImport&,
			Composition::HjsonImport::FSemanticWorkspaceChangeProjection&&,
			FRootPropertyApplyOutcome&)> TryApply,
		TFunctionRef<bool(FString&, FString&, FWorkspaceError&)> TryReadback,
		FString& OutPayloadJson,
		FWorkspaceError& OutError)
	{
		using namespace Composition::HjsonImport;

		const FString RequestId = ImportContract.RequestPrefix + TEXT("-import-")
			+ FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
		const FString IdempotencyKey = ImportContract.RequestPrefix + TEXT(":")
			+ Workspace->GetWorkspaceFingerprint();
		const Contract::FUeAgentCallerContext Caller(
			ImportContract.RequestPrefix + TEXT("-import"),
			{TEXT("uai:query"), TEXT("uai:operate"), TEXT("authoring.write"),
				TEXT("authoring.structure")},
			RequestId);
		FSemanticWorkspaceChangeProjection Projection;
		FPreparedAuthoringImportPtr Prepared;
		FPreviewWorkspaceApply Preview;
		FSemanticWorkspaceImportError ImportError;
		if (!TryPrepareSemanticWorkspaceChanges(
				Workspace, CurrentModel, CurrentSnapshot, MoveTemp(DescriptorIdentity),
				RequestId, IdempotencyKey, Caller, ExpectedRootOwnerPath, Resources,
				Projection, Prepared, ImportError)
			|| !Prepared.IsValid())
		{
			return Fail(OutError, ImportError.IsValid() ? ImportError
				: UE::UAI::Contract::FDiagnostic::Failure(ImportContract.PrepareFailedErrorCode,
					TEXT("The import could not be prepared."), TEXT("preflight")), ImportContract.TargetIdentity);
		}
		if (!Preview.Prepare(*Prepared, Projection, OutError)) return false;
		if (!Projection.HasChanges())
		{
			// Prepare the response before mutating the preview owner.
			const auto Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("status"), TEXT("succeeded"));
			if (!TryCanonicalizeObject(Result, OutPayloadJson, OutError)) return false;
			if (!Preview.Apply(OutError))
			{
				FWorkspaceError RecoveryError;
				if (!Preview.Rollback(RecoveryError)) OutError = MoveTemp(RecoveryError);
				return false;
			}
			// Preview-only has no asset compile/save/reload lifecycle.
			return true;
		}

		UeAgentAuthoringWorkspaceTransaction::FScopedDurableRecoveryNotice RecoveryNotice;
		FRootPropertyApplyOutcome ApplyOutcome;
		bool bFinalReadbackCompleted = false;
		ApplyOutcome.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure)
		{
			FWorkspaceError ReadError;
			bFinalReadbackCompleted = TryReadbackImportPayload(
				ImportContract.ProviderId, ImportContract.TargetIdentity, ImportContract.ObjectPath,
				nullptr, ImportContract.ReadbackPayloadErrorCode,
				ImportContract.CompileNotSettledErrorCode, TryReadback, OutPayloadJson, ReadError);
			if (bFinalReadbackCompleted)
				bFinalReadbackCompleted = AppendRecoveryNotice(RecoveryNotice, OutPayloadJson, ReadError);
			if (!bFinalReadbackCompleted)
			{
				Failure.Diagnostic = ReadError.GetDiagnostic();
			}
			return bFinalReadbackCompleted;
		};
		UeAgentNativeNormalizationIntent::FScope NativeNormalization;
		const auto AssetReadback = ApplyOutcome.FinalReadback;
		const auto IntentDocumentSources = Projection.DocumentSourceBindings;
		AssetPreview::FEditorStateContinuity PreviewContinuity;
		PreviewContinuity.Capture();
		ApplyOutcome.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure)
		{
			UE::UAI::Contract::FDiagnostic ContinuityError;
			if (!PreviewContinuity.Restore(ContinuityError))
			{
				Failure.Diagnostic = PreviewRestoreFailure(ImportContract.TargetIdentity, ContinuityError).GetDiagnostic();
				return false;
			}
			FWorkspaceError PreviewError;
			if (Preview.HasChanges() && !Preview.Apply(PreviewError))
			{
				Failure.Diagnostic = PreviewError.GetDiagnostic();
				return false;
			}
			const bool bHasValueIntent = Prepared->GetChangeSetDraft().GetChanges().ContainsByPredicate([](const auto& Change)
			{
				return !AssetPreview::IsPreviewSemantic(Change.GetIdentity().GetSemanticId())
					&& Change.GetExpectedBefore().IsValid() && Change.GetProposedAfter().IsValid()
					&& !Change.GetIdentity().GetSelector(CanonicalAuthoringTargetIdentitySelector).IsEmpty();
			}) || !NativeNormalization.GetCreatedProperties().IsEmpty();
			if (ImportContract.BuildFinalReadRequests && bHasValueIntent)
			{
				TArray<UeAgentAuthoringState::FReadRequest> Requests;
				Contract::FDiagnostic CaptureError;
				UeAgentAuthoringState::FCaptureResult Capture;
				if (!ImportContract.BuildFinalReadRequests(Requests, CaptureError)
					|| !UeAgentAuthoringState::CaptureSnapshot(Requests, Capture, nullptr, nullptr,
						UeAgentAuthoringState::ECaptureValueMode::Native))
				{
					UeAgentAuthoringDiagnostics::ReportConsistencyWarning(TEXT("domain_hjson_final_capture_failed"),
						CaptureError.IsEmpty() ? Capture.Diagnostic : CaptureError, TEXT("final_intent_readback"));
				}
				else
				{
					FWorkspaceError IntentError;
					if (!TryVerifySemanticWorkspaceValueIntent(*Prepared, Capture.Snapshot, IntentError, IntentDocumentSources,
						&NativeNormalization, &Requests, ApplyOutcome.GraphReferenceBindings))
						UeAgentAuthoringDiagnostics::ReportConsistencyWarning(IntentError.GetCode(),
							IntentError.GetDiagnostic(), TEXT("final_intent_readback"));
				}
			}
			if (!AssetReadback(Failure)) return false;
			FWorkspaceError NoticeError;
			if (!AppendWarnings(PreviewContinuity.GetAdjustments(), OutPayloadJson, NoticeError))
			{
				bFinalReadbackCompleted = false;
				Failure.Diagnostic = NoticeError.GetDiagnostic();
				return false;
			}
			return true;
		};
		const auto AssetChanges = Preview.HasChanges()
			? Prepared->SelectChanges([](const PlanningPreflight::FChangeDraft& Change)
				{ return !AssetPreview::IsPreviewSemantic(Change.GetIdentity().GetSemanticId()); })
			: Prepared;
		const TArray<FString> ApplySourceUnitIds = Projection.ChangedUnitIds;
		const auto ApplyDocumentSources = Projection.DocumentSourceBindings;
		TArray<FString> ApplyDocumentIdentities;
		for (const auto& Change : AssetChanges->GetChangeSetDraft().GetChanges())
			if (Change.GetIdentity().GetSemanticId() != TEXT("core.property"))
			{
				ApplyDocumentIdentities.AddUnique(Change.GetIdentity().GetCanonicalKey());
				const FString AuthoringIdentity = Change.GetIdentity().GetSelector(CanonicalAuthoringTargetIdentitySelector);
				if (!AuthoringIdentity.IsEmpty()) ApplyDocumentIdentities.AddUnique(AuthoringIdentity);
			}
		if (!TryApply(RequestId, *AssetChanges, MoveTemp(Projection), ApplyOutcome)
			|| !ApplyOutcome.bApplied)
		{
			{
				auto Diagnostic = ApplyOutcome.Diagnostic;
				if (Diagnostic.IsEmpty()) Diagnostic = Contract::FDiagnostic::Failure(
					ImportContract.ApplyFailedErrorCode, TEXT("The asset operation failed without a diagnostic."), ApplyOutcome.Phase);
				if (Diagnostic.Stage.IsEmpty()) Diagnostic.Stage = ApplyOutcome.Phase;
				TArray<Contract::FDiagnostic*> Pending{&Diagnostic};
				while (!Pending.IsEmpty())
				{
					auto& Issue = *Pending.Pop(EAllowShrinking::No);
					if (Issue.TargetPath.IsEmpty() || Issue.TargetPath.StartsWith(TEXT("/Temp/UAI/")))
						Issue.TargetPath = ImportContract.TargetIdentity;
					if (Issue.SourceKind != TEXT("native_result") && Issue.SourceKind != TEXT("command_input"))
					{
						Workspace->LocateDocumentDiagnostic(ApplyDocumentIdentities, ApplyDocumentSources, Issue);
						if (Issue.Source.SourceFile.IsEmpty() || Issue.Source.SourceFile.StartsWith(TEXT("inline://")))
						{
							FWorkspaceError Located;
							Located.SetFailure(Issue);
							Located.LocateUnits(Workspace->GetUnitSources());
							Issue = Located.GetDiagnostic();
							if (!Issue.Source.SourceFile.IsEmpty()) Issue.SourceKind = TEXT("related_input");
						}
					}
					for (auto& Child : Issue.Diagnostics) Pending.Add(&Child);
					if (Issue.PublicCause.IsValid())
					{
						TSharedPtr<Contract::FDiagnostic> Cause = MakeShared<Contract::FDiagnostic>(*Issue.PublicCause);
						Issue.PublicCause = Cause;
						Pending.Add(Cause.Get());
					}
				}
				// Preserve the first precise failure location when the aggregate has none.
				if (Diagnostic.Source.SourceFile.IsEmpty() || Diagnostic.Source.SourceFile.StartsWith(TEXT("inline://")))
					for (const auto& Issue : Diagnostic.Diagnostics)
					{
						if (Issue.Source.SourceFile.IsEmpty() || Issue.Source.SourceFile.StartsWith(TEXT("inline://"))) continue;
						Diagnostic.Source = Issue.Source;
						Diagnostic.SemanticPointer = Issue.SemanticPointer;
						Diagnostic.SourceKind = Issue.SourceKind;
						Diagnostic.RelatedSources = Issue.RelatedSources;
						break;
					}

				// A legacy provider may report only a code, not a field pointer. Retain
				// the authored unit context, and explicitly leave field attribution unchecked.
				// Multiple changed units are contexts, never fabricated error locations.
				if (Diagnostic.Source.SourceFile.IsEmpty() || Diagnostic.Source.SourceFile.StartsWith(TEXT("inline://")))
				{
					for (const FString& UnitId : ApplySourceUnitIds)
					{
						const auto* Source = Workspace->GetUnitSources().Find(UnitId);
						if (!Source) continue;
						const auto Context = MakeShared<FJsonObject>();
						Context->SetStringField(TEXT("stage"), ApplyOutcome.Phase);
						Context->SetStringField(TEXT("reason"), TEXT("provider_field_location_not_reported"));
						Context->SetStringField(TEXT("source_file"), Source->GetSourceUnit());
						Context->SetStringField(TEXT("json_pointer"), Source->GetJsonPointer());
						Context->SetNumberField(TEXT("line"), Source->GetLine());
						Context->SetNumberField(TEXT("column"), Source->GetColumn());
						Diagnostic.Unchecked.Add(MakeShared<FJsonValueObject>(Context));
					}
				}
				OutError.SetFailure(MoveTemp(Diagnostic));
			}
			UE::UAI::Contract::FDiagnostic ContinuityError;
			if (!PreviewContinuity.Restore(ContinuityError))
				OutError.Append(PreviewRestoreFailure(ImportContract.TargetIdentity, ContinuityError));
			FWorkspaceError RecoveryError;
			if (!Preview.Rollback(RecoveryError)) OutError.Append(RecoveryError);
			return false;
		}
		if (!bFinalReadbackCompleted)
		{
			auto Error = NativeFailure(TEXT("domain_hjson_atomic_readback_not_executed"), TEXT("The semantic workspace operation reported applied without completing its required final readback."), TEXT("/final_readback"));
			Error.Stage = ApplyOutcome.Phase;
			Error.TargetPath = ImportContract.TargetIdentity;
			Error.MutationStarted = true;
			Error.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			Error.ActualValue = MakeShared<FJsonValueBoolean>(false);
			return Fail(OutError, MoveTemp(Error));
		}
		return true;
	}

	bool TryReadCandidateSelector(
		const TSharedPtr<const FJsonObject>& Candidate,
		const FString& JsonPointer,
		FDomainHjsonCandidateSelector& OutSelector,
		FWorkspaceError& OutError)
	{
		OutSelector = {};
		OutError.Reset();
		if (!Candidate.IsValid())
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("domain_hjson_candidate_selector_invalid"), TEXT("The candidate selector must be an object with a non-empty name."), TEXT("domain_authoring"));
			Error.ActualValuePresent = false;
			return Fail(OutError, MoveTemp(Error), FString(), JsonPointer);
		}

		const FString NamePointer = JsonPointer + TEXT("/name");
		FString RawName;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Candidate->Values)
		{
			if (Field.Key != TEXT("name") && Field.Key != TEXT("select"))
			{
				auto Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("domain_hjson_candidate_selector_field_unsupported"), TEXT("Candidate selectors support only name and select fields."), TEXT("domain_authoring"));
				Error.ActualValue = MakeShared<FJsonValueString>(Field.Key);
				return Fail(OutError, MoveTemp(Error), FString(), JsonPointer + TEXT("/") + EscapeJsonPointerSegment(Field.Key));
			}
		}

		auto TryReadStrictString = [&Candidate](
			const TCHAR* FieldName,
			FString& OutValue)
		{
			const TSharedPtr<FJsonValue> Value = Candidate->TryGetField(FieldName);
			if (!Value.IsValid())
			{
				return true;
			}
			if (Value->Type != EJson::String)
			{
				return false;
			}
			OutValue = Value->AsString();
			return true;
		};

		if (!TryReadStrictString(TEXT("name"), RawName))
		{
			return Fail(OutError, UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("domain_hjson_candidate_selector_invalid"), NamePointer,
				MakeShared<FJsonValueString>(TEXT("string")), MakeShared<FJsonValueString>(UeAgentJsonDiagnostics::JsonValueTypeToString(Candidate->TryGetField(TEXT("name")))),
				TEXT("domain_authoring"), TEXT("The candidate name must be a string.")), FString(), NamePointer);
		}
		OutSelector.Name = RawName.TrimStartAndEnd();
		if (OutSelector.Name.IsEmpty())
		{
			return Fail(OutError, UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("domain_hjson_candidate_selector_required"), NamePointer,
				MakeShared<FJsonValueString>(TEXT("non-empty string")), Candidate->TryGetField(TEXT("name")), TEXT("domain_authoring"),
				TEXT("The candidate selector requires a non-empty name.")), FString(), NamePointer);
		}

		const TSharedPtr<FJsonObject>* Select = nullptr;
		if (Candidate->HasField(TEXT("select")))
		{
			if (!Candidate->TryGetObjectField(TEXT("select"), Select)
				|| Select == nullptr || !Select->IsValid())
			{
				return Fail(OutError, UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("domain_hjson_candidate_selector_select_invalid"), JsonPointer + TEXT("/select"),
					MakeShared<FJsonValueString>(TEXT("object")), MakeShared<FJsonValueString>(UeAgentJsonDiagnostics::JsonValueTypeToString(Candidate->TryGetField(TEXT("select")))),
					TEXT("domain_authoring"), TEXT("Candidate select must be an object containing one or two semantic fields.")), FString(), JsonPointer + TEXT("/select"));
			}
			if ((*Select)->Values.IsEmpty() || (*Select)->Values.Num() > 2)
			{
				return Fail(OutError, UE::UAI::Contract::FDiagnostic::Failure(
					TEXT("domain_hjson_candidate_selector_select_invalid"),
					FString::Printf(TEXT("Candidate select requires one or two semantic fields; received %d."), (*Select)->Values.Num()),
					TEXT("domain_authoring")), FString(), JsonPointer + TEXT("/select"));
			}
			TSet<FString> SeenSelectKeys;
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Field :
				(*Select)->Values)
			{
				const FString Key = Field.Key.TrimStartAndEnd();
				const FString LowerKey = Key.ToLower();
				FString Value;
				if (Key.IsEmpty()
					|| SeenSelectKeys.Contains(LowerKey)
					|| !Field.Value.IsValid()
					|| Field.Value->Type != EJson::String
					|| !Field.Value->TryGetString(Value))
				{
					const TCHAR* Reason = Key.IsEmpty() ? TEXT("A candidate select field name cannot be empty.") : SeenSelectKeys.Contains(LowerKey)
						? TEXT("Candidate select field names must be unique after trimming and case normalization.") : TEXT("A candidate select value must be a string.");
					auto Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("domain_hjson_candidate_selector_select_invalid"), Reason, TEXT("domain_authoring"));
					Error.CandidateName = OutSelector.Name;
					if (Key.IsEmpty() || SeenSelectKeys.Contains(LowerKey)) Error.ActualValue = MakeShared<FJsonValueString>(Field.Key);
					else Error.ActualValue = MakeShared<FJsonValueString>(UeAgentJsonDiagnostics::JsonValueTypeToString(Field.Value));
					return Fail(OutError, MoveTemp(Error), FString(), JsonPointer + TEXT("/select/") + EscapeJsonPointerSegment(Field.Key));
				}
				SeenSelectKeys.Add(LowerKey);
				Value.TrimStartAndEndInline();
				if (Value.IsEmpty()
					|| LowerKey == TEXT("id")
					|| LowerKey == TEXT("class_path")
					|| LowerKey == TEXT("factory_ref")
					|| LowerKey.Contains(TEXT("guid"))
					|| LowerKey.Contains(TEXT("hash"))
					|| LowerKey.Contains(TEXT("digest"))
					|| LowerKey.Contains(TEXT("fingerprint"))
					|| LowerKey.Contains(TEXT("revision")))
				{
					auto Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("domain_hjson_candidate_selector_select_field_unsupported"),
						Value.IsEmpty() ? TEXT("A candidate semantic disambiguation value cannot be empty.") : TEXT("Candidate select must use semantic fields rather than machine identities, class paths or revision fields."), TEXT("domain_authoring"));
					Error.CandidateName = OutSelector.Name;
					if (Value.IsEmpty()) Error.ActualValue = MakeShared<FJsonValueString>(FString());
					else Error.ActualEvidence = Field.Value;
					return Fail(OutError, MoveTemp(Error), FString(), JsonPointer + TEXT("/select/") + EscapeJsonPointerSegment(Field.Key));
				}
				OutSelector.Select.Add(Key, MoveTemp(Value));
			}
		}
		if (!OutSelector.IsValid())
		{
			return Fail(OutError, UE::UAI::Contract::FDiagnostic::Failure(TEXT("domain_hjson_candidate_selector_required"),
				TEXT("A candidate selector requires a non-empty name and at most two non-empty semantic select fields."), TEXT("domain_authoring")), FString(), JsonPointer);
		}
		return true;
	}
}

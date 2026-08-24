#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"

#include "Algo/Unique.h"
#include "Containers/StringConv.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/SourceLoader/UeAgentHjsonSourceLoader.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/UObjectGlobals.h"

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
			FString Code,
			FString Source = FString(),
			FString Pointer = TEXT("/"))
		{
			OutError.Set(MoveTemp(Code), MoveTemp(Source), MoveTemp(Pointer));
			return false;
		}

		bool IsSha256(const FString& Value)
		{
			if (!Value.StartsWith(TEXT("sha256:")) || Value.Len() != 71)
			{
				return false;
			}
			for (int32 Index = 7; Index < Value.Len(); ++Index)
			{
				if (!FChar::IsHexDigit(Value[Index]))
				{
					return false;
				}
			}
			return true;
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

		bool IsSafeEntryFile(const FString& EntryFile)
		{
			if (EntryFile.IsEmpty()
				|| !FPaths::IsRelative(EntryFile)
				|| EntryFile.Contains(TEXT("\\"))
				|| EntryFile.Contains(TEXT(":"))
				|| EntryFile.StartsWith(TEXT("/"))
				|| !EntryFile.EndsWith(TEXT(".hjson"), ESearchCase::IgnoreCase)
				|| EntryFile.Equals(
					TEXT(".uai/workspace.hjson"), ESearchCase::IgnoreCase))
			{
				return false;
			}
			TArray<FString> Segments;
			EntryFile.ParseIntoArray(Segments, TEXT("/"), false);
			if (Segments.IsEmpty())
			{
				return false;
			}
			for (const FString& Segment : Segments)
			{
				if (Segment.IsEmpty()
					|| Segment == TEXT(".")
					|| Segment == TEXT(".."))
				{
					return false;
				}
			}
			return true;
		}

		FUeAgentResourceTracker MakeResources()
		{
			return FUeAgentResourceTracker(FUeAgentResourceBudget({
				{EUeAgentResourceDimension::InputBytes, 16 * 1024 * 1024},
				{EUeAgentResourceDimension::OutputBytes, 16 * 1024 * 1024},
				{EUeAgentResourceDimension::StringBytes, 16 * 1024 * 1024},
				{EUeAgentResourceDimension::Nodes, 65536},
				{EUeAgentResourceDimension::Items, 65536},
				{EUeAgentResourceDimension::Files, 4096},
				{EUeAgentResourceDimension::ReferenceEdges, 65536},
				{EUeAgentResourceDimension::WorkUnits, 262144},
				{EUeAgentResourceDimension::EstimatedMemoryBytes,
					512ll * 1024 * 1024},
			}));
		}

		TSharedPtr<FJsonValue> JsonValueFromHjson(
			const FHjsonNode& Node,
			FWorkspaceError& OutError)
		{
			switch (Node.GetKind())
			{
			case EHjsonNodeKind::Null:
				return MakeShared<FJsonValueNull>();
			case EHjsonNodeKind::Boolean:
				return MakeShared<FJsonValueBoolean>(Node.GetBoolean());
			case EHjsonNodeKind::String:
				return MakeShared<FJsonValueString>(Node.GetText());
			case EHjsonNodeKind::Integer:
			case EHjsonNodeKind::Decimal:
				return MakeShared<FJsonValueNumber>(FCString::Atod(*Node.GetLexeme()));
			case EHjsonNodeKind::Array:
			{
				TArray<TSharedPtr<FJsonValue>> Values;
				for (const FHjsonNodeRef& Item : Node.GetItems())
				{
					TSharedPtr<FJsonValue> Value = JsonValueFromHjson(*Item, OutError);
					if (!Value.IsValid()) return nullptr;
					Values.Add(MoveTemp(Value));
				}
				return MakeShared<FJsonValueArray>(MoveTemp(Values));
			}
			case EHjsonNodeKind::Object:
			{
				const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
				for (const TPair<FString, FHjsonNodeRef>& Entry : Node.GetEntries())
				{
					TSharedPtr<FJsonValue> Value =
						JsonValueFromHjson(*Entry.Value, OutError);
					if (!Value.IsValid()) return nullptr;
					Object->SetField(Entry.Key, MoveTemp(Value));
				}
				return MakeShared<FJsonValueObject>(Object);
			}
			default:
				Fail(OutError, TEXT("domain_hjson_node_kind_invalid"));
				return nullptr;
			}
		}

		void AddFieldState(
			const FUeAgentContentGraphFieldState& State,
			const TSharedPtr<FJsonObject>& Value)
		{
			FString ConversionError;
			TSharedPtr<FJsonValue> StateValue;
			if (State.Value.IsValid()
				&& StateSemantic::CanonicalJson::TryToJsonValue(
					*State.Value, StateValue, ConversionError)
				&& StateValue.IsValid())
			{
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
		}

		void AddCanonicalState(
			const StateSemantic::FUeAgentCanonicalValuePtr& State,
			const TSharedPtr<FJsonObject>& Value)
		{
			FString ConversionError;
			TSharedPtr<FJsonValue> StateValue;
			if (State.IsValid()
				&& StateSemantic::CanonicalJson::TryToJsonValue(
					*State, StateValue, ConversionError)
				&& StateValue.IsValid())
			{
				Value->SetField(TEXT("value"), StateValue);
			}
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
		return !DomainId.IsEmpty()
			&& !SchemaId.IsEmpty()
			&& !RootTypeIdentities.IsEmpty();
	}

	bool FDomainHjsonDocumentContract::SupportsRootType(
		const FString& InRootTypeIdentity) const
	{
		return RootTypeIdentities.Contains(InRootTypeIdentity);
	}

	bool FDomainHjsonImportLifecycleReceipt::IsComplete() const
	{
		return bMutationApplied
			&& bCompileSettled
			&& bPackageSaved
			&& bUnloadedReloaded
			&& !PersistenceReceiptId.TrimStartAndEnd().IsEmpty();
	}

	bool FDomainHjsonCandidateBinding::IsValid() const
	{
		return !Name.TrimStartAndEnd().IsEmpty()
			&& !Kind.TrimStartAndEnd().IsEmpty()
			&& !SupportedOperations.IsEmpty()
			&& !InputContract.TrimStartAndEnd().IsEmpty()
			&& !PlacementContract.TrimStartAndEnd().IsEmpty()
			&& !RequiredContext.TrimStartAndEnd().IsEmpty()
			&& !BindingRevision.TrimStartAndEnd().IsEmpty();
	}

	bool FDomainHjsonCandidateBinding::SupportsOperation(
		const FString& Operation) const
	{
		return SupportedOperations.Contains(Operation);
	}

	bool TryDecodeResolvedDocument(
		const FString& RequestTargetIdentity,
		const FString& CanonicalParamsJson,
		const FDomainHjsonDocumentContract& Contract,
		FResolvedDomainHjsonDocument& OutDocument,
		FWorkspaceError& OutError)
	{
		OutDocument = {};
		OutError.Reset();
		if (!Contract.IsValid())
		{
			return Fail(OutError, TEXT("domain_hjson_contract_invalid"));
		}
		TSharedPtr<FJsonObject> Params;
		FString JsonError;
		if (!CommandContract::Json::TryParseObject(
			CanonicalParamsJson, Params, JsonError) || !Params.IsValid())
		{
			return Fail(
				OutError, TEXT("domain_hjson_params_invalid"), JsonError);
		}
		if (Params->HasField(TEXT("carrier"))
			|| Params->HasField(TEXT("hjson_document")))
		{
			return Fail(
				OutError, TEXT("domain_hjson_legacy_source_contract_rejected"));
		}
		FString Target;
		FString ExpectedRevision;
		const TSharedPtr<FJsonObject>* Source = nullptr;
		FString Kind;
		FString Carrier;
		FString SourceUnit;
		FString SourceTarget;
		FString BaseRevision;
		FString EntryFile;
		FString SourceFingerprint;
		FString SemanticFingerprint;
		const TArray<TSharedPtr<FJsonValue>>* Documents = nullptr;
		if (!Params->TryGetStringField(TEXT("target"), Target)
			|| Target.IsEmpty()
			|| Target != RequestTargetIdentity
			|| !Params->TryGetStringField(
				TEXT("expected_revision"), ExpectedRevision)
			|| ExpectedRevision.IsEmpty())
		{
			return Fail(
				OutError, TEXT("domain_hjson_target_or_revision_invalid"));
		}
		if (!Params->TryGetObjectField(TEXT("source"), Source)
			|| !Source || !Source->IsValid()
			|| !(*Source)->TryGetStringField(TEXT("kind"), Kind)
			|| Kind != TEXT("resolved")
			|| !(*Source)->TryGetStringField(TEXT("carrier"), Carrier)
			|| !(*Source)->TryGetStringField(TEXT("source_unit"), SourceUnit)
			|| SourceUnit.IsEmpty()
			|| !(*Source)->TryGetStringField(TEXT("target_identity"), SourceTarget)
			|| !(*Source)->TryGetStringField(TEXT("base_revision"), BaseRevision)
			|| !(*Source)->TryGetStringField(TEXT("entry_file"), EntryFile)
			|| EntryFile.IsEmpty()
			|| !(*Source)->TryGetStringField(
				TEXT("source_fingerprint"), SourceFingerprint)
			|| !(*Source)->TryGetStringField(
				TEXT("semantic_fingerprint"), SemanticFingerprint)
			|| !IsSha256(SourceFingerprint)
			|| !IsSha256(SemanticFingerprint)
			|| !(*Source)->TryGetArrayField(TEXT("documents"), Documents)
			|| !Documents || Documents->IsEmpty())
		{
			return Fail(OutError, TEXT("domain_hjson_resolved_source_invalid"));
		}
		if (SourceTarget != Target)
		{
			return Fail(
				OutError, TEXT("domain_hjson_target_identity_mismatch"), SourceTarget);
		}
		if (Carrier == TEXT("directory"))
		{
			if (BaseRevision != ExpectedRevision)
			{
				return Fail(
					OutError,
					TEXT("domain_hjson_base_revision_mismatch"),
					BaseRevision);
			}
		}
		else if ((Carrier != TEXT("inline")
				&& Carrier != TEXT("single_file"))
			|| !BaseRevision.IsEmpty())
		{
			return Fail(
				OutError, TEXT("domain_hjson_resolved_source_invalid"), Carrier);
		}
		FString DocumentText;
		int32 EntryMatches = 0;
		for (const TSharedPtr<FJsonValue>& Value : *Documents)
		{
			const TSharedPtr<FJsonObject> Document =
				Value.IsValid() ? Value->AsObject() : nullptr;
			FString Path;
			FString Text;
			FString ContentHash;
			if (!Document.IsValid()
				|| !Document->TryGetStringField(TEXT("path"), Path)
				|| !Document->TryGetStringField(TEXT("document"), Text)
				|| !Document->TryGetStringField(TEXT("content_hash"), ContentHash)
				|| !IsSha256(ContentHash))
			{
				return Fail(
					OutError, TEXT("domain_hjson_resolved_source_invalid"));
			}
			if (Path.Equals(EntryFile, ESearchCase::IgnoreCase))
			{
				DocumentText = MoveTemp(Text);
				++EntryMatches;
			}
		}
		if (EntryMatches > 1)
		{
			return Fail(
				OutError, TEXT("domain_hjson_entry_document_ambiguous"), EntryFile);
		}
		if (EntryMatches != 1 || DocumentText.IsEmpty())
		{
			return Fail(
				OutError, TEXT("domain_hjson_entry_document_missing"), EntryFile);
		}
		FUeAgentResourceTracker Resources = MakeResources();
		FHjsonDocumentPtr Document;
		if (!TryParseHjsonDocument(
			DocumentText,
			TEXT("resolved:") + Contract.DomainId + TEXT(":") + EntryFile,
			Resources,
			Document,
			OutError)
			|| !Document.IsValid())
		{
			return false;
		}
		TSharedPtr<FJsonValue> Converted =
			JsonValueFromHjson(*Document->GetRoot(), OutError);
		const TSharedPtr<FJsonObject> Canonical =
			Converted.IsValid() ? Converted->AsObject() : nullptr;
		FString Schema;
		const TSharedPtr<FJsonObject>* Root = nullptr;
		FString RootType;
		if (!Canonical.IsValid()
			|| !Canonical->TryGetStringField(TEXT("schema"), Schema))
		{
			return Fail(OutError, TEXT("domain_hjson_schema_missing"));
		}
		if (Schema != Contract.SchemaId)
		{
			return Fail(
				OutError, TEXT("domain_hjson_schema_mismatch"), Schema);
		}
		if (!Canonical->TryGetObjectField(TEXT("root"), Root)
			|| !Root || !Root->IsValid()
			|| !(*Root)->TryGetStringField(TEXT("type"), RootType))
		{
			return Fail(OutError, TEXT("domain_hjson_root_missing"));
		}
		if (!Contract.SupportsRootType(RootType))
		{
			return Fail(
				OutError, TEXT("domain_hjson_root_type_mismatch"), RootType);
		}
		OutDocument.TargetIdentity = MoveTemp(Target);
		OutDocument.ExpectedRevision = MoveTemp(ExpectedRevision);
		OutDocument.Carrier = MoveTemp(Carrier);
		OutDocument.SourceUnit = MoveTemp(SourceUnit);
		OutDocument.BaseRevision = MoveTemp(BaseRevision);
		OutDocument.EntryFile = MoveTemp(EntryFile);
		OutDocument.SourceFingerprint = MoveTemp(SourceFingerprint);
		OutDocument.SemanticFingerprint = MoveTemp(SemanticFingerprint);
		OutDocument.Params = MoveTemp(Params);
		OutDocument.Root = *Root;
		return true;
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
			return Fail(
				OutError, TEXT("domain_hjson_target_unresolved"), TargetIdentity);
		}
		if (!OutObject->GetClass()
			|| !ExpectedRootTypeIdentities.Contains(
				OutObject->GetClass()->GetPathName()))
		{
			return Fail(
				OutError, TEXT("domain_hjson_target_type_mismatch"),
				OutObject->GetClass() ? OutObject->GetClass()->GetPathName() : FString());
		}
		FUeAgentContentGraphReadError CaptureError;
		if (!FUeAgentContentGraphReadProductionEnvironment::TryCaptureOwner(
			*OutObject, OutCapture, CaptureError))
		{
			return Fail(
				OutError,
				CaptureError.Code.IsEmpty()
					? FString(TEXT("domain_hjson_capture_failed"))
					: CaptureError.Code,
				CaptureError.SubjectId,
				CaptureError.Detail);
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
			return Fail(OutError, TEXT("domain_hjson_capture_identity_invalid"));
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
			AddCanonicalState(Item.State, Value);
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
			AddFieldState(Property.State, Value);
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
			AddFieldState(Parameter.State, Value);
			Parameters.Add(MakeShared<FJsonValueObject>(Value));
		}
		Result->SetArrayField(TEXT("parameters"), MoveTemp(Parameters));

		bool bValid = true;
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
			bValid = bValid && (Diagnostic.bResolved
				|| !Diagnostic.Severity.Equals(TEXT("error"), ESearchCase::IgnoreCase));
		}
		Result->SetArrayField(TEXT("diagnostics"), MoveTemp(Diagnostics));
		Result->SetStringField(
			TEXT("compile_status"), bValid ? TEXT("valid") : TEXT("error"));
		OutSnapshot = Result;
		OutError.Reset();
		return true;
	}

	bool TryCanonicalizeObject(
		const TSharedPtr<FJsonObject>& Object,
		FString& OutJson,
		FWorkspaceError& OutError)
	{
		FString Error;
		if (!Object.IsValid()
			|| !CommandContract::Json::TryCanonicalize(Object, OutJson, Error))
		{
			return Fail(
				OutError, TEXT("domain_hjson_json_invalid"), Error);
		}
		return true;
	}

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
		OutPlan.Reset();
		OutError.Reset();
		if (TargetIdentity.IsEmpty()
			|| !Contract.IsValid()
			|| BaseRevision.IsEmpty()
			|| ProfileId.IsEmpty()
			|| ProviderId.IsEmpty()
			|| !Document.IsValid())
		{
			return Fail(OutError, TEXT("domain_hjson_export_identity_invalid"));
		}
		if (!IsSafeEntryFile(EntryFile))
		{
			return Fail(
				OutError, TEXT("domain_hjson_export_entry_invalid"), EntryFile);
		}
		FString Schema;
		const TSharedPtr<FJsonObject>* Root = nullptr;
		FString RootType;
		if (!Document->TryGetStringField(TEXT("schema"), Schema)
			|| Schema != Contract.SchemaId)
		{
			return Fail(
				OutError, TEXT("domain_hjson_export_schema_mismatch"), Schema);
		}
		if (!Document->TryGetObjectField(TEXT("root"), Root)
			|| !Root || !Root->IsValid()
			|| !(*Root)->TryGetStringField(TEXT("type"), RootType)
			|| !Contract.SupportsRootType(RootType))
		{
			return Fail(
				OutError, TEXT("domain_hjson_export_root_type_mismatch"), RootType);
		}

		FString CanonicalDocument;
		if (!TryCanonicalizeObject(Document, CanonicalDocument, OutError))
		{
			return false;
		}
		TArray<uint8> EntryBytes = Utf8(CanonicalDocument);
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
		FUeAgentResourceBudgetExceeded ResourceError;
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
			OutError.Set(
				ResourceError.Code,
				ResourceError.Path,
				TEXT("/"),
				ResourceError.Observed,
				ResourceError.bStoppedEarly);
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
		if (StagedWorkspacePath.IsEmpty()
			|| TargetIdentity.IsEmpty()
			|| !Contract.IsValid()
			|| BaseRevision.IsEmpty()
			|| !IsSafeEntryFile(EntryFile))
		{
			return Fail(OutError, TEXT("domain_hjson_staging_identity_invalid"));
		}
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
			return Fail(
				OutError, TEXT("domain_hjson_staging_identity_mismatch"));
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
			return Fail(
				OutError, TEXT("domain_hjson_staging_identity_mismatch"));
		}
		return true;
	}

	void SetWorkspaceErrorFromCommandFailure(
		const FString& ErrorJson,
		FString FallbackCode,
		FString FallbackSubject,
		FWorkspaceError& OutError)
	{
		TSharedPtr<FJsonObject> ErrorObject;
		FString ParseError;
		if (CommandContract::Json::TryParseObject(
				ErrorJson, ErrorObject, ParseError)
			&& ErrorObject.IsValid())
		{
			FString Code;
			FString Subject;
			FString Detail;
			ErrorObject->TryGetStringField(TEXT("code"), Code);
			ErrorObject->TryGetStringField(TEXT("subject_id"), Subject);
			ErrorObject->TryGetStringField(TEXT("detail"), Detail);
			Code.TrimStartAndEndInline();
			Subject.TrimStartAndEndInline();
			if (!Code.IsEmpty())
			{
				OutError.Set(
					MoveTemp(Code),
					Subject.IsEmpty()
						? MoveTemp(FallbackSubject)
						: MoveTemp(Subject),
					Detail.IsEmpty() ? TEXT("/") : MoveTemp(Detail));
				return;
			}
		}
		OutError.Set(
			MoveTemp(FallbackCode),
			MoveTemp(FallbackSubject),
			ErrorJson.IsEmpty() ? MoveTemp(ParseError) : ErrorJson);
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
			return Fail(OutError, TEXT("domain_hjson_result_identity_invalid"));
		}
		if (!LifecycleReceipt.IsComplete())
		{
			return Fail(
				OutError, TEXT("domain_hjson_lifecycle_receipt_incomplete"));
		}
		const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(
			TEXT("command_id"), TEXT("content_graph.import_workspace"));
		Result->SetStringField(TEXT("terminal_status"), TEXT("succeeded"));
		Result->SetStringField(TEXT("provider_id"), ProviderId);
		Result->SetStringField(TEXT("target"), TargetIdentity);
		Result->SetStringField(TEXT("revision"), Revision);
		Result->SetBoolField(
			TEXT("mutation_applied"), LifecycleReceipt.bMutationApplied);
		Result->SetBoolField(
			TEXT("compile_settled"), LifecycleReceipt.bCompileSettled);
		Result->SetBoolField(TEXT("saved"), LifecycleReceipt.bPackageSaved);
		Result->SetBoolField(
			TEXT("reloaded"), LifecycleReceipt.bUnloadedReloaded);
		Result->SetStringField(
			TEXT("persistence_receipt_id"),
			LifecycleReceipt.PersistenceReceiptId);
		Result->SetObjectField(TEXT("independent_readback"), IndependentReadback);
		if (Mutation.IsValid())
		{
			Result->SetObjectField(TEXT("mutation"), Mutation);
		}
		return TryCanonicalizeObject(Result, OutPayloadJson, OutError);
	}

	bool TryReadCandidateBinding(
		const TSharedPtr<FJsonObject>& Candidate,
		const FString& JsonPointer,
		FDomainHjsonCandidateBinding& OutBinding,
		FWorkspaceError& OutError)
	{
		OutBinding = {};
		const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
		if (!Candidate.IsValid()
			|| !Candidate->TryGetStringField(TEXT("name"), OutBinding.Name)
			|| !Candidate->TryGetStringField(TEXT("kind"), OutBinding.Kind)
			|| !Candidate->TryGetArrayField(
				TEXT("supported_operations"), Operations)
			|| !Operations
			|| !Candidate->TryGetStringField(
				TEXT("input_contract"), OutBinding.InputContract)
			|| !Candidate->TryGetStringField(
				TEXT("placement_contract"), OutBinding.PlacementContract)
			|| !Candidate->TryGetStringField(
				TEXT("required_context"), OutBinding.RequiredContext)
			|| !Candidate->TryGetStringField(
				TEXT("binding_revision"), OutBinding.BindingRevision))
		{
			return Fail(
				OutError,
				TEXT("domain_hjson_candidate_binding_invalid"),
				FString(),
				JsonPointer);
		}
		for (const TSharedPtr<FJsonValue>& Value : *Operations)
		{
			FString Operation;
			if (!Value.IsValid()
				|| !Value->TryGetString(Operation)
				|| Operation.TrimStartAndEnd().IsEmpty()
				|| OutBinding.SupportedOperations.Contains(Operation))
			{
				return Fail(
					OutError,
					TEXT("domain_hjson_candidate_binding_invalid"),
					OutBinding.Name,
					JsonPointer + TEXT("/supported_operations"));
			}
			OutBinding.SupportedOperations.Add(MoveTemp(Operation));
		}
		if (!OutBinding.IsValid())
		{
			return Fail(
				OutError,
				TEXT("domain_hjson_candidate_binding_invalid"),
				OutBinding.Name,
				JsonPointer);
		}
		OutError.Reset();
		return true;
	}
}

#pragma once

#include "CoreMinimal.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::HjsonWorkspace
{
	using UE::UAI::ContextResource::FUeAgentResourceTracker;
	using UE::UAI::StateSemantic::FUeAgentCanonicalSnapshotRef;
	using UE::UAI::StateSemantic::FUeAgentStateRevision;

	enum class EHjsonNodeKind : uint8
	{
		Null,
		Boolean,
		String,
		Integer,
		Decimal,
		Array,
		Object,
	};

	enum class EWorkspaceSourceKind : uint8
	{
		Directory,
		SingleFile,
		Inline,
	};

	enum class EWorkspaceKind : uint8
	{
		Directory,
		SingleFile,
		Inline,
	};

	class UEAGENTINTERFACE_API FWorkspaceError final
	{
	public:
		const FString& GetCode() const;
		const FString& GetSourceUnit() const;
		const FString& GetJsonPointer() const;
		int64 GetObserved() const;
		bool IsStoppedEarly() const;
		const TArray<FString>& GetCyclePath() const;

		void Reset();
		void Set(
			FString InCode,
			FString InSourceUnit = FString(),
			FString InJsonPointer = TEXT("/"),
			int64 InObserved = 0,
			bool bInStoppedEarly = false,
			TArray<FString> InCyclePath = {});

	private:
		FString Code;
		FString SourceUnit;
		FString JsonPointer = TEXT("/");
		int64 Observed = 0;
		bool bStoppedEarly = false;
		TArray<FString> CyclePath;
	};

	class UEAGENTINTERFACE_API FSourceProvenance final
	{
	public:
		FSourceProvenance(
			FString InSourceUnit,
			FString InJsonPointer,
			int32 InLine,
			int32 InColumn,
			int64 InByteStart,
			int64 InByteEnd,
			FString InOriginalKey);

		const FString& GetSourceUnit() const;
		const FString& GetJsonPointer() const;
		int32 GetLine() const;
		int32 GetColumn() const;
		int64 GetByteStart() const;
		int64 GetByteEnd() const;
		const FString& GetOriginalKey() const;

	private:
		FString SourceUnit;
		FString JsonPointer;
		int32 Line = 1;
		int32 Column = 1;
		int64 ByteStart = 0;
		int64 ByteEnd = 0;
		FString OriginalKey;
	};

	class FHjsonNode;
	using FHjsonNodePtr = TSharedPtr<const FHjsonNode>;
	using FHjsonNodeRef = TSharedRef<const FHjsonNode>;

	class UEAGENTINTERFACE_API FHjsonNode final
	{
	public:
		FHjsonNode(
			EHjsonNodeKind InKind,
			FSourceProvenance InProvenance,
			FString InLexeme,
			FString InText,
			bool bInBoolean,
			TArray<FHjsonNodeRef> InItems,
			TArray<TPair<FString, FHjsonNodeRef>> InEntries);

		EHjsonNodeKind GetKind() const;
		const FSourceProvenance& GetProvenance() const;
		const FString& GetLexeme() const;
		const FString& GetText() const;
		bool GetBoolean() const;
		const TArray<FHjsonNodeRef>& GetItems() const;
		const TArray<TPair<FString, FHjsonNodeRef>>& GetEntries() const;
		const FHjsonNode* Get(const FString& Key) const;

	private:
		EHjsonNodeKind Kind = EHjsonNodeKind::Null;
		FSourceProvenance Provenance;
		FString Lexeme;
		FString Text;
		bool bBoolean = false;
		TArray<FHjsonNodeRef> Items;
		TArray<TPair<FString, FHjsonNodeRef>> Entries;
	};

	class FHjsonDocument;
	using FHjsonDocumentPtr = TSharedPtr<const FHjsonDocument>;
	using FHjsonDocumentRef = TSharedRef<const FHjsonDocument>;

	class UEAGENTINTERFACE_API FHjsonDocument final
	{
	public:
		FHjsonDocument(FString InSourceUnit, FHjsonNodeRef InRoot);

		const FString& GetSourceUnit() const;
		const FHjsonNodeRef& GetRoot() const;
		const FHjsonNode* Find(const FString& JsonPointer) const;

	private:
		FString SourceUnit;
		FHjsonNodeRef Root;
	};

	class UEAGENTINTERFACE_API FWorkspaceSource final
	{
	public:
		static FWorkspaceSource Directory(FString Path);
		static FWorkspaceSource SingleFile(FString Path);
		static FWorkspaceSource Inline(
			FString Text,
			FString SourceUnit,
			FString SourceId);

		EWorkspaceSourceKind GetKind() const;
		const FString& GetPath() const;
		const FString& GetText() const;
		const FString& GetSourceUnit() const;
		const FString& GetSourceId() const;

	private:
		FWorkspaceSource(
			EWorkspaceSourceKind InKind,
			FString InPath,
			FString InText,
			FString InSourceUnit,
			FString InSourceId);

		EWorkspaceSourceKind Kind = EWorkspaceSourceKind::Inline;
		FString Path;
		FString Text;
		FString SourceUnit;
		FString SourceId;
	};

	class UEAGENTINTERFACE_API FWorkspacePolicy final
	{
	public:
		FWorkspacePolicy(
			FString InDialectId,
			FString InContractId,
			int32 InContractVersion,
			FString InProfileId,
			FString InEntryUnit,
			TArray<FString> InReferenceFields,
			int32 InMaxFiles);

		const FString& GetDialectId() const;
		const FString& GetContractId() const;
		int32 GetContractVersion() const;
		const FString& GetProfileId() const;
		const FString& GetEntryUnit() const;
		const TArray<FString>& GetReferenceFields() const;
		int32 GetMaxFiles() const;
		bool IsValid() const;

	private:
		FString DialectId;
		FString ContractId;
		int32 ContractVersion = 0;
		FString ProfileId;
		FString EntryUnit;
		TArray<FString> ReferenceFields;
		int32 MaxFiles = 0;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API FWorkspaceUnit final
	{
	public:
		FWorkspaceUnit(
			FString InUnitId,
			FHjsonDocumentRef InDocument,
			TArray<uint8> InCanonicalUtf8,
			FString InContentHash);

		const FString& GetUnitId() const;
		const FHjsonDocumentRef& GetDocument() const;
		const TArray<uint8>& GetCanonicalUtf8() const;
		const FString& GetContentHash() const;

	private:
		FString UnitId;
		FHjsonDocumentRef Document;
		TArray<uint8> CanonicalUtf8;
		FString ContentHash;
	};

	class UEAGENTINTERFACE_API FReferenceEdge final
	{
	public:
		FReferenceEdge(
			FString InSourceUnit,
			FString InJsonPointer,
			FString InTargetUnit);

		const FString& GetSourceUnit() const;
		const FString& GetJsonPointer() const;
		const FString& GetTargetUnit() const;

	private:
		FString SourceUnit;
		FString JsonPointer;
		FString TargetUnit;
	};

	class FCanonicalWorkspace;
	using FCanonicalWorkspacePtr = TSharedPtr<const FCanonicalWorkspace>;
	using FCanonicalWorkspaceRef = TSharedRef<const FCanonicalWorkspace>;

	class UEAGENTINTERFACE_API FCanonicalWorkspace final
	{
	public:
		FCanonicalWorkspace(
			EWorkspaceSourceKind InSourceKind,
			FWorkspacePolicy InPolicy,
			TArray<FWorkspaceUnit> InUnits,
			TArray<FReferenceEdge> InReferenceEdges,
			FString InSemanticFingerprint);

		EWorkspaceSourceKind GetSourceKind() const;
		const FWorkspacePolicy& GetPolicy() const;
		const TArray<FWorkspaceUnit>& GetUnits() const;
		const TArray<FReferenceEdge>& GetReferenceEdges() const;
		const FString& GetSemanticFingerprint() const;

	private:
		EWorkspaceSourceKind SourceKind = EWorkspaceSourceKind::Inline;
		FWorkspacePolicy Policy;
		TArray<FWorkspaceUnit> Units;
		TArray<FReferenceEdge> ReferenceEdges;
		FString SemanticFingerprint;
	};

	class UEAGENTINTERFACE_API FExportPlanRequest final
	{
	public:
		FExportPlanRequest(
			FUeAgentCanonicalSnapshotRef InSnapshot,
			EWorkspaceKind InWorkspaceKind,
			FString InEntryFile,
			FString InContractId,
			int32 InContractVersion,
			FString InProfileId,
			int64 InTokenBudget);

		const FUeAgentCanonicalSnapshotRef& GetSnapshot() const;
		EWorkspaceKind GetWorkspaceKind() const;
		const FString& GetEntryFile() const;
		const FString& GetContractId() const;
		int32 GetContractVersion() const;
		const FString& GetProfileId() const;
		int64 GetTokenBudget() const;
		bool IsValid() const;

	private:
		FUeAgentCanonicalSnapshotRef Snapshot;
		EWorkspaceKind WorkspaceKind = EWorkspaceKind::Directory;
		FString EntryFile;
		FString ContractId;
		int32 ContractVersion = 0;
		FString ProfileId;
		int64 TokenBudget = 0;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API FExportFilePlan final
	{
	public:
		FExportFilePlan(
			FString InRelativePath,
			TArray<uint8> InCanonicalUtf8,
			FString InContentHash);

		const FString& GetRelativePath() const;
		const TArray<uint8>& GetCanonicalUtf8() const;
		const FString& GetContentHash() const;

	private:
		FString RelativePath;
		TArray<uint8> CanonicalUtf8;
		FString ContentHash;
	};

	class FExportWorkspacePlan;
	using FExportWorkspacePlanPtr = TSharedPtr<const FExportWorkspacePlan>;
	using FExportWorkspacePlanRef = TSharedRef<const FExportWorkspacePlan>;

	class UEAGENTINTERFACE_API FExportWorkspacePlan final
	{
	public:
		FExportWorkspacePlan(
			EWorkspaceKind InWorkspaceKind,
			FString InEntryFile,
			FString InSnapshotId,
			FUeAgentStateRevision InSnapshotRevision,
			FString InSnapshotEvidenceId,
			TArray<FExportFilePlan> InFiles,
			FString InPlanFingerprint,
			int64 InObservedTokens);

		EWorkspaceKind GetWorkspaceKind() const;
		const FString& GetEntryFile() const;
		const FString& GetSnapshotId() const;
		const FUeAgentStateRevision& GetSnapshotRevision() const;
		const FString& GetSnapshotEvidenceId() const;
		const TArray<FExportFilePlan>& GetFiles() const;
		const FString& GetPlanFingerprint() const;
		int64 GetObservedTokens() const;

	private:
		EWorkspaceKind WorkspaceKind = EWorkspaceKind::Directory;
		FString EntryFile;
		FString SnapshotId;
		FUeAgentStateRevision SnapshotRevision;
		FString SnapshotEvidenceId;
		TArray<FExportFilePlan> Files;
		FString PlanFingerprint;
		int64 ObservedTokens = 0;
	};

	UEAGENTINTERFACE_API bool TryParseHjsonDocument(
		const FString& Text,
		const FString& SourceUnit,
		FUeAgentResourceTracker& Resources,
		FHjsonDocumentPtr& OutDocument,
		FWorkspaceError& OutError);

	UEAGENTINTERFACE_API bool TryWriteHjsonDocument(
		FHjsonDocumentRef Document,
		FUeAgentResourceTracker& Resources,
		TArray<uint8>& OutCanonicalUtf8,
		FWorkspaceError& OutError);

	UEAGENTINTERFACE_API bool TryLoadWorkspace(
		const FWorkspaceSource& Source,
		const FWorkspacePolicy& Policy,
		FUeAgentResourceTracker& Resources,
		FCanonicalWorkspacePtr& OutWorkspace,
		FWorkspaceError& OutError);

	UEAGENTINTERFACE_API bool TryBuildExportPlan(
		const FExportPlanRequest& Request,
		FUeAgentResourceTracker& Resources,
		FExportWorkspacePlanPtr& OutPlan,
		FWorkspaceError& OutError);
}

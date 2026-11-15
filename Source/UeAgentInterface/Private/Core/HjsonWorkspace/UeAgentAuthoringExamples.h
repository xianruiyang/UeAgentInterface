#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"

namespace UE::UAI::HjsonWorkspace
{
	class FExportFilePlan;
	class FWorkspaceError;
	struct FKeyedItemProjectionContract;
	struct FAuthoringExamplePublicationProof;

	/** Reading aids only. No example is an import default, requirement or identity. */
	struct FAuthoringExample
	{
		FString Name;
		TSharedPtr<FJsonValue> Value;
		TArray<FString> Notes;
	};

	using FAuthoringExampleDefinitionResolver = TFunction<TSharedPtr<FJsonObject>(const FString&)>;

	/** Types recurse through the existing descriptors. Special rules supply only their own value shape. */
	UEAGENTINTERFACE_API TSharedRef<FJsonObject> BuildAuthoringTypeExamples(
		const TSharedRef<FJsonObject>& Definition,
		const FAuthoringExampleDefinitionResolver& ResolveDefinition);

	/** The keyed contract remains the authority for new-item eligibility and public shape. */
	TArray<FAuthoringExample> BuildKeyedItemExamples(const FKeyedItemProjectionContract& Contract,
		const TSharedPtr<FJsonObject>& RecordSchema = nullptr);

	/** Domain rules return values; the shared catalog owns layout, formatting, deduplication and navigation. */
	using FAuthoringDocumentExampleRule = TFunction<TArray<FAuthoringExample>(
		const FString& SemanticType, const FString& Role, const FString& Field)>;

	class FAuthoringExampleCatalog
	{
	public:
		struct FPropertyInstruction
		{
			FString Pointer, TypeName, Definition, Category, Name;
		};
		/** Same property guide consumer, without manufacturing a document/value tree. */
		void AddPropertyDocument(const FString& Path, TArray<FPropertyInstruction>&& Properties);
		void AddDefinitions(const FString& Path, const TSharedRef<FJsonObject>& Definitions,
			const TSharedPtr<FJsonObject>& Enums = nullptr);
		void AddDocument(const FString& Path, const StateSemantic::FUeAgentCanonicalValueRef& Document,
			const FString& SemanticType = {}, const FString& Role = {}, const FString& Field = {});
		void AddRule(FAuthoringDocumentExampleRule Rule);
		void AddCollection(const FString& Path, const StateSemantic::FUeAgentCanonicalValueRef& Document,
			const FKeyedItemProjectionContract& Contract);
		/** Collect the declared aliases consulted by the document guide reader. */
		static void CollectPropertyGuideReferences(const StateSemantic::FUeAgentCanonicalValueRef& Document,
			TSet<FString>& OutReferences);
		bool Publish(TArray<FExportFilePlan>& Files, FWorkspaceError& OutError,
			TFunctionRef<FString(const TArray<uint8>&)> Hash,
			const TSharedPtr<const FAuthoringExamplePublicationProof>& Previous = nullptr,
			TSharedPtr<const FAuthoringExamplePublicationProof>* OutProof = nullptr,
			const TMap<FString, const FExportFilePlan*>* PreviousFiles = nullptr,
			const TSharedPtr<FJsonObject>& PreviousIndex = nullptr,
			const TSet<FString>* ReplacedDocuments = nullptr,
			const TMap<FString, StateSemantic::FUeAgentCanonicalValueRef>* PreviousGuides = nullptr,
			const TMap<FString, TArray<FString>>* ReplacedPointers = nullptr,
			TMap<FString, StateSemantic::FUeAgentCanonicalValueRef>* ProducedGuides = nullptr);
	private:
		struct FDocument
		{
			FString Path, SemanticType, Role, Field;
			StateSemantic::FUeAgentCanonicalValuePtr Value;
			TArray<FPropertyInstruction> Properties;
		};
		TMap<FString, TSharedPtr<FJsonObject>> Definitions;
		TMap<FString, TSharedRef<FJsonObject>> DefinitionCatalogs;
		TArray<FDocument> Documents;
		struct FCollection
		{
			FString Path;
			StateSemantic::FUeAgentCanonicalValueRef Document;
			TSharedPtr<const FKeyedItemProjectionContract> Contract;
		};
		TArray<FCollection> Collections;
		TArray<FAuthoringDocumentExampleRule> Rules;
	};

	void RegisterDomainAuthoringExampleRules(FAuthoringExampleCatalog& Catalog);
	TArray<FAuthoringExample> BuildDomainKeyedItemExamples(const FKeyedItemProjectionContract& Contract);
}

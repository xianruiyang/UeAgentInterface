#include "Domains/Blackboard/Authoring/UeAgentBlackboardDocument.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringJsonFields.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType.h"
#include "Composition/HjsonImport/UeAgentSemanticWorkspaceImport.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/HjsonWorkspace/UeAgentKeyedItemAuthoringProjection.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceIO.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Domains/Blackboard/Authoring/UeAgentBlackboardAuthoringState.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "QueryOwners/AiBehavior/UeAgentAiBehaviorCandidateQuery.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"
#include "Utilities/AuthoringObjectApply/UeAgentAuthoringObjectApply.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/Class.h"

namespace UeAgentBlackboardAuthoring
{
	namespace
	{
		bool Fail(UE::UAI::Contract::FDiagnostic& OutError, const UE::UAI::Contract::FDiagnostic& Failure)
        {
            OutError = Failure;
            return false;
        }

		FString KeyPointer(const FString& Name)
		{
			return TEXT("/blackboard_keys/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(Name);
		}

		UE::UAI::Contract::FDiagnostic ValueFailure(const TCHAR* Code, const TCHAR* Reason,
			const FString& Path, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
		{
			return UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Path, Expected, Actual, TEXT("blackboard_authoring"), Reason);
		}

		TSharedPtr<FJsonValue> JsonType(const TSharedPtr<FJsonValue>& Value)
		{
			if (!Value) return nullptr;
			const TCHAR* Type = Value->Type == EJson::Object ? TEXT("object") : Value->Type == EJson::Array ? TEXT("array")
				: Value->Type == EJson::String ? TEXT("string") : Value->Type == EJson::Number ? TEXT("number")
				: Value->Type == EJson::Boolean ? TEXT("boolean") : Value->Type == EJson::Null ? TEXT("null") : TEXT("none");
			return MakeShared<FJsonValueString>(Type);
		}

		UBlackboardData* ResolveParent(const FString& PackageName)
		{
			if (PackageName.IsEmpty()) return nullptr;
			const FString ObjectPath = PackageName + TEXT(".")
				+ FPackageName::GetLongPackageAssetName(PackageName);
			return LoadObject<UBlackboardData>(nullptr, *ObjectPath);
		}

		bool BuildKeyProperties(
			const TArray<UeAgentAuthoringState::FStateEntry>& Entries,
			const FString& KeyName,
			TSharedPtr<FJsonObject>& OutProperties,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			TArray<UeAgentAuthoringState::FStateEntry> Selected;
			for (const auto& Entry : Entries)
			{
				if (Entry.SemanticId == KeyPropertySemanticId
					&& Entry.TargetIdentity.GetSelector(TEXT("key_name")) == KeyName)
				{
					Selected.Add(Entry);
				}
			}
			return UeAgentDetailsAuthoringService::BuildPropertiesDocument(
				Selected, {}, OutProperties, OutError);
		}

		bool ResolveCandidateClass(
			UBlackboardData& Blackboard,
			const TSharedPtr<FJsonObject>& Candidate,
			UClass*& OutClass,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutClass = nullptr;
			UE::UAI::HjsonWorkspace::DomainAuthoring::FDomainHjsonCandidateSelector Selector;
			UE::UAI::HjsonWorkspace::FWorkspaceError SelectionError;
			if (!UE::UAI::HjsonWorkspace::DomainAuthoring::TryReadCandidateSelector(
				Candidate, TEXT("/candidate"), Selector, SelectionError))
				return Fail(OutError, SelectionError.GetDiagnostic());
			UE::UAI::QueryOwners::AiBehavior::FAiBehaviorCandidate Resolved;
			UE::UAI::QueryOwners::AiBehavior::FAiBehaviorCandidateQueryError Error;
			if (!UE::UAI::QueryOwners::AiBehavior::TryResolveAiBehaviorCandidate(
					Blackboard, Selector.Name, FString(), Resolved, Error, Selector.Select))
			{
				return Fail(OutError, Error);
			}
			if (Resolved.Kind != TEXT("blackboard_key"))
				return Fail(OutError, ValueFailure(TEXT("blackboard_candidate_invalid"),
					TEXT("The selected candidate is not a blackboard key."),
					TEXT("/candidate"), MakeShared<FJsonValueString>(Resolved.Kind), MakeShared<FJsonValueString>(TEXT("supported blackboard_key candidate"))));
			if (!Resolved.bSupported)
			{
				auto Diagnostic = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("blackboard_candidate_invalid"), Resolved.SupportDiagnostic);
				UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(Diagnostic, TEXT("/candidate"));
				return Fail(OutError, Diagnostic);
			}
			OutClass = LoadObject<UClass>(nullptr, *Resolved.PayloadType);
			if (!OutClass || !OutClass->IsChildOf(UBlackboardKeyType::StaticClass()))
			{
				auto Diagnostic = ValueFailure(TEXT("blackboard_candidate_type_invalid"),
					!OutClass ? TEXT("The candidate payload class could not be loaded.") : TEXT("The candidate payload class does not derive from BlackboardKeyType."),
					TEXT("/candidate"), MakeShared<FJsonValueString>(Resolved.PayloadType), MakeShared<FJsonValueString>(UBlackboardKeyType::StaticClass()->GetPathName()));
				Diagnostic.CandidateName = Resolved.Name;
				return Fail(OutError, Diagnostic);
			}
			return true;
		}

		bool BuildNewKey(UBlackboardData& Context, const FString& Name,
			const TSharedPtr<FJsonObject>& Source, TSharedPtr<FJsonObject>& OutItem, UE::UAI::Contract::FDiagnostic& OutError,
			const UE::UAI::HjsonWorkspace::FHjsonNode* InitialCategorySource)
		{
			using namespace UE::UAI;
			const TSharedPtr<FJsonObject>* Candidate = nullptr;
			UClass* Class = nullptr;
			if (!Source->TryGetObjectField(TEXT("candidate"), Candidate))
				return Fail(OutError, ValueFailure(TEXT("blackboard_candidate_required"), TEXT("Creating or changing a blackboard key type requires a candidate object."),
					TEXT("/candidate"), JsonType(Source->TryGetField(TEXT("candidate"))), MakeShared<FJsonValueString>(TEXT("object"))));
			if (!ResolveCandidateClass(Context, *Candidate, Class, OutError)) return false;
			TStrongObjectPtr<UBlackboardData> Template(NewObject<UBlackboardData>(GetTransientPackage()));
			Template->Keys.Reset();
			FBlackboardEntry& Entry = Template->Keys.AddDefaulted_GetRef();
			Entry.EntryName = FName(*Name);
			Entry.KeyType = NewObject<UBlackboardKeyType>(Template.Get(), Class, NAME_None, RF_Transactional);
			TSharedPtr<FJsonValue> Defaults;
			if (!ReadDocument(*Template, Defaults, OutError)) return false;
			OutItem = Defaults->AsObject()->GetObjectField(TEXT("blackboard_keys"))->GetObjectField(Name);
			const TSharedPtr<FJsonObject>* Categories = nullptr;
			if (!Source->HasField(TEXT("categories"))) return true;
			if (!Source->TryGetObjectField(TEXT("categories"), Categories))
				return Fail(OutError, ValueFailure(TEXT("hjson_initial_categories_invalid"), TEXT("Initial key categories must be an object."),
					TEXT("/categories"), JsonType(Source->TryGetField(TEXT("categories"))), MakeShared<FJsonValueString>(TEXT("object"))));
			TArray<UeAgentAuthoringState::FReadRequest> Requests;
			if (!AppendStructuralReadRequests(*Template, Template->GetPathName(), Requests, OutError)) return false;
			Requests.RemoveAll([](const auto& Request) { return Request.SemanticId != KeyPropertySemanticId; });
			UeAgentAuthoringState::FCaptureResult Capture;
			if (!UeAgentAuthoringState::CaptureSnapshot(Requests, Capture))
				return Fail(OutError, Capture.Diagnostic);
			auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
			FString Text;
			HjsonWorkspace::FHjsonDocumentPtr Parsed;
			HjsonWorkspace::FWorkspaceError Error;
			if (!InitialCategorySource && !CommandContract::Json::TryCanonicalize(*Categories, Text, OutError)) return false;
			if (!InitialCategorySource && !HjsonWorkspace::TryParseHjsonDocument(Text, Name, Resources, Parsed, Error))
				return Fail(OutError, Error.GetDiagnostic());
			TArray<TSharedPtr<FJsonValue>> Changes;
			Composition::HjsonImport::FSemanticWorkspaceImportError Failure;
			if (!Composition::HjsonImport::TryProjectReflectedPropertyValues(Capture.Snapshot,
				InitialCategorySource ? *InitialCategorySource : *Parsed->GetRoot(), Resources, Changes, Failure))
				return Fail(OutError, Failure);
			const auto Properties = OutItem->GetObjectField(TEXT("properties"));
			TArray<TSharedPtr<FJsonValue>> InitialDescriptors;
			for (const auto& Change : Changes)
			{
				const FString Path = Change->AsObject()->GetStringField(TEXT("property_path"));
				const auto* Descriptor = Properties->GetArrayField(TEXT("properties")).FindByPredicate([&](const auto& Value) {
					return Value->AsObject()->GetStringField(TEXT("property_path")) == Path;
				});
				if (!Descriptor)
				{
					auto Diagnostic = ValueFailure(TEXT("blackboard_initial_property_missing"), TEXT("The initial property is absent from the selected key type's captured properties."),
						Path, MakeShared<FJsonValueString>(Path), MakeShared<FJsonValueString>(TEXT("property exposed by the selected key type")));
					UeAgentAuthoringDiagnostics::LocateAuthoringSource(Diagnostic, *Change->AsObject());
					return Fail(OutError, Diagnostic);
				}
				const auto Initial = UeAgentAuthoringState::CloneCanonicalValue(*Descriptor);
				Initial->AsObject()->SetField(TEXT("value"), UeAgentAuthoringState::CloneCanonicalValue(Change->AsObject()->TryGetField(TEXT("value"))));
				if (const auto SourceLocation = Change->AsObject()->TryGetField(TEXT("authoring_source")))
					Initial->AsObject()->SetField(TEXT("authoring_source"), UeAgentAuthoringState::CloneCanonicalValue(SourceLocation));
				InitialDescriptors.Add(Initial);
			}
			Properties->SetArrayField(TEXT("properties"), InitialDescriptors);
			UeAgentAuthoringObjectApply::FOptions Options;
			Options.bShowDefaultsOnly = true; Options.bCompactValues = true; Options.bEmitEditorNotifications = true;
			UeAgentAuthoringDirectApply::FResult Result;
			if (!UeAgentDetailsAuthoringService::ApplyObjectProperties(Entry.KeyType, Properties, Options, Result)) { OutError = Result.Diagnostic; return false; }
			if (!ReadDocument(*Template, Defaults, OutError)) return false;
			OutItem = Defaults->AsObject()->GetObjectField(TEXT("blackboard_keys"))->GetObjectField(Name);
			HjsonWorkspace::AttachProjectedPropertySources(Changes, *OutItem->GetObjectField(TEXT("properties")));
			return true;
		}
	}

	bool ValidateDocument(const TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutError.Reset();
		const TSharedPtr<FJsonObject>* Keys = nullptr;
		if (!Value.IsValid() || Value->Type != EJson::Object)
			return Fail(OutError, ValueFailure(TEXT("blackboard_document_keys_invalid"), TEXT("The blackboard document must be an object."),
				TEXT("/"), JsonType(Value), MakeShared<FJsonValueString>(TEXT("object"))));
		if (!Value->AsObject()->TryGetObjectField(TEXT("blackboard_keys"), Keys))
			return Fail(OutError, ValueFailure(TEXT("blackboard_document_keys_invalid"), TEXT("The blackboard document requires a blackboard_keys object."),
				TEXT("/blackboard_keys"), JsonType(Value->AsObject()->TryGetField(TEXT("blackboard_keys"))), MakeShared<FJsonValueString>(TEXT("object"))));
		for (const auto& Pair : (*Keys)->Values)
		{
			if (Pair.Key.TrimStartAndEnd().IsEmpty())
				return Fail(OutError, ValueFailure(TEXT("blackboard_document_key_invalid"), TEXT("A blackboard key name cannot be blank."),
					KeyPointer(Pair.Key), MakeShared<FJsonValueString>(Pair.Key), MakeShared<FJsonValueString>(TEXT("nonblank key name"))));
			if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::Object)
				return Fail(OutError, ValueFailure(TEXT("blackboard_document_key_invalid"), TEXT("A blackboard key definition must be an object."),
					KeyPointer(Pair.Key), JsonType(Pair.Value), MakeShared<FJsonValueString>(TEXT("object"))));
			const auto Item = Pair.Value->AsObject();
			if (!UeAgentAuthoringJsonFields::ValidateOptionalFields(*Item,
				{{TEXT("name"), EJson::String}, {TEXT("key_type_class"), EJson::String}, {TEXT("candidate"), EJson::Object},
				 {TEXT("description"), EJson::String}, {TEXT("category"), EJson::String}, {TEXT("instance_synced"), EJson::Boolean},
				 {TEXT("properties"), EJson::Object}}, OutError,
				TEXT("/blackboard_keys/") + Pair.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")))) return false;
			FString ClassPath;
			const TSharedPtr<FJsonObject>* Candidate = nullptr;
			if ((!Item->TryGetStringField(TEXT("key_type_class"), ClassPath) || ClassPath.IsEmpty())
				&& !Item->TryGetObjectField(TEXT("candidate"), Candidate))
			{
				return Fail(OutError, ValueFailure(TEXT("blackboard_document_key_type_missing"), TEXT("The key requires a nonempty key_type_class or a candidate object."),
					KeyPointer(Pair.Key) + TEXT("/key_type_class"), Item->TryGetField(TEXT("key_type_class")), MakeShared<FJsonValueString>(TEXT("class path or candidate object"))));
			}
		}
		return true;
	}

	bool ReadDocument(UBlackboardData& Blackboard, TSharedPtr<FJsonValue>& OutValue, UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutValue.Reset();
		OutError.Reset();
		TArray<UeAgentAuthoringState::FReadRequest> Requests;
		if (!AppendStructuralReadRequests(
				Blackboard, Blackboard.GetOutermost()->GetName(), Requests, OutError)) return false;
		// Exact document retains complete captured values; the ordinary projection applies visibility later.
		for (auto& Request : Requests) Request.Limits.StructFieldFilter = nullptr;
		UeAgentAuthoringState::FCaptureResult Capture;
		if (!UeAgentAuthoringState::CaptureSnapshot(Requests, Capture))
		{
			return Fail(OutError, Capture.Diagnostic);
		}
		const auto Entries = Capture.Snapshot.CopyEntries();
		const auto Document = MakeShared<FJsonObject>();
		Document->SetStringField(TEXT("parent"), Blackboard.Parent
			? Blackboard.Parent->GetOutermost()->GetName() : FString());
#if WITH_EDITORONLY_DATA
		TArray<TSharedPtr<FJsonValue>> Inherited;
		for (const FBlackboardEntry& Entry : Blackboard.ParentKeys)
		{
			const auto Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("name"), Entry.EntryName.ToString());
			Item->SetStringField(TEXT("key_type"), Entry.KeyType
				? Entry.KeyType->GetClass()->GetDisplayNameText().ToString() : FString());
			Inherited.Add(MakeShared<FJsonValueObject>(Item));
		}
		Document->SetArrayField(TEXT("inherited_keys"), Inherited);
#endif
		const auto Keys = MakeShared<FJsonObject>();
		int32 Index = 0;
		for (const FBlackboardEntry& Entry : Blackboard.Keys)
		{
			const FString KeyName = Entry.EntryName.ToString();
			const auto Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("name"), KeyName);
			Item->SetNumberField(TEXT("index"), Index++);
			Item->SetStringField(TEXT("key_type"), Entry.KeyType
				? Entry.KeyType->GetClass()->GetDisplayNameText().ToString() : FString());
			Item->SetStringField(TEXT("key_type_class"), Entry.KeyType
				? Entry.KeyType->GetClass()->GetPathName() : FString());
#if WITH_EDITORONLY_DATA
			Item->SetStringField(TEXT("description"), Entry.EntryDescription);
			Item->SetStringField(TEXT("category"), Entry.EntryCategory.ToString());
#endif
			Item->SetBoolField(TEXT("instance_synced"), Entry.bInstanceSynced != 0);
			TSharedPtr<FJsonObject> Properties;
			if (!BuildKeyProperties(Entries, KeyName, Properties, OutError)) return false;
			Item->SetObjectField(TEXT("properties"), Properties);
			Keys->SetObjectField(KeyName, Item);
		}
		Document->SetObjectField(TEXT("blackboard_keys"), Keys);
		OutValue = MakeShared<FJsonValueObject>(Document);
		return true;
	}

	bool ApplyDocument(UBlackboardData& Blackboard, const TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!ValidateDocument(Value, OutError)) return false;
		const auto Document = Value->AsObject();
		FString ParentPath;
		if (!UeAgentAuthoringJsonFields::ReadReferenceField(*Document, TEXT("parent"), ParentPath, OutError)) return false;
		UBlackboardData* Parent = ResolveParent(ParentPath);
		if (!ParentPath.IsEmpty() && !Parent)
			return Fail(OutError, ValueFailure(TEXT("blackboard_parent_not_found"), TEXT("The parent reference could not be loaded as a blackboard asset."),
				TEXT("/parent"), MakeShared<FJsonValueString>(ParentPath), MakeShared<FJsonValueString>(TEXT("loadable BlackboardData asset"))));
		if (Parent && (Parent == &Blackboard || Parent->IsChildOf(Blackboard)))
			return Fail(OutError, ValueFailure(TEXT("blackboard_parent_cycle"), TEXT("The selected parent is this blackboard or one of its descendants, which would create an inheritance cycle."),
				TEXT("/parent"), MakeShared<FJsonValueString>(ParentPath), MakeShared<FJsonValueString>(TEXT("parent outside this blackboard's descendant chain"))));

		TArray<FBlackboardEntry> Desired;
		const auto Keys = Document->GetObjectField(TEXT("blackboard_keys"));
		TSet<FName> Names;
		TArray<FString> OrderedNames;
		Keys->Values.GetKeys(OrderedNames);
		TMap<FString, FString> OrderLexemes;
		for (const FString& Name : OrderedNames)
		{
			const auto Item = Keys->GetObjectField(Name);
			FString Index = FString::FromInt(MAX_int32);
			if (Item->HasField(TEXT("index")))
			{
				double FiniteValue = 0;
				int32 Comparison = 0;
				const auto IndexValue = Item->TryGetField(TEXT("index"));
				if (!IndexValue || !UE::UAI::StateSemantic::CanonicalJson::TryGetNumberLexeme(*IndexValue, Index)
					|| !UE::UAI::StateSemantic::IsIntegralNumberLexeme(Index)
					|| !UE::UAI::StateSemantic::TryParseFiniteDecimal(Index, FiniteValue)
					|| !UE::UAI::StateSemantic::TryCompareNumberLexemes(Index, TEXT("0"), Comparison) || Comparison < 0)
				{
					OutError = ValueFailure(TEXT("blackboard_key_index_invalid"), TEXT("The key index must be a finite nonnegative integer."),
						KeyPointer(Name) + TEXT("/index"), IndexValue, MakeShared<FJsonValueString>(TEXT("finite integer >= 0")));
					UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Item, TEXT("input_decode"), FString(), TEXT("index"));
					return false;
				}
			}
			OrderLexemes.Add(Name, MoveTemp(Index));
		}
		OrderedNames.Sort([&](const FString& Left, const FString& Right) {
			int32 Comparison = 0;
			UE::UAI::StateSemantic::TryCompareNumberLexemes(OrderLexemes.FindChecked(Left), OrderLexemes.FindChecked(Right), Comparison);
			return Comparison != 0 ? Comparison < 0 : Left < Right;
		});
		for (const FString& Name : OrderedNames)
		{
			const auto Item = Keys->GetObjectField(Name);
			FString NativeName = Name;
			Item->TryGetStringField(TEXT("name"), NativeName);
			const FName EntryName(*NativeName);
			if (EntryName.IsNone() || Names.Contains(EntryName))
				return Fail(OutError, ValueFailure(TEXT("blackboard_key_name_duplicate"),
					EntryName.IsNone() ? TEXT("The key name resolves to the reserved None name.") : TEXT("Another key already uses this name under Unreal's name comparison."),
					KeyPointer(Name) + (Item->HasField(TEXT("name")) ? TEXT("/name") : TEXT("")),
					MakeShared<FJsonValueString>(NativeName), MakeShared<FJsonValueString>(TEXT("unique non-None key name"))));
			Names.Add(EntryName);
			FString ClassPath;
			Item->TryGetStringField(TEXT("key_type_class"), ClassPath);
			UClass* KeyClass = ClassPath.IsEmpty() ? nullptr : LoadObject<UClass>(nullptr, *ClassPath);
			if (!KeyClass)
			{
				const TSharedPtr<FJsonObject>* Candidate = nullptr;
				if (!Item->TryGetObjectField(TEXT("candidate"), Candidate))
					return Fail(OutError, ValueFailure(TEXT("blackboard_key_type_unresolved"), TEXT("The key class could not be loaded and no candidate object is available to resolve it."),
						KeyPointer(Name) + TEXT("/key_type_class"), MakeShared<FJsonValueString>(ClassPath), MakeShared<FJsonValueString>(TEXT("loadable key class or candidate object"))));
				if (!ResolveCandidateClass(Blackboard, *Candidate, KeyClass, OutError))
				{
					UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(OutError, KeyPointer(Name));
					UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Item);
					return false;
				}
			}
			if (!KeyClass->IsChildOf(UBlackboardKeyType::StaticClass())
				|| KeyClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
			{
				const TCHAR* Reason = !KeyClass->IsChildOf(UBlackboardKeyType::StaticClass()) ? TEXT("The key class does not derive from BlackboardKeyType.")
					: KeyClass->HasAnyClassFlags(CLASS_Abstract) ? TEXT("The key class is abstract and cannot be instantiated.")
					: KeyClass->HasAnyClassFlags(CLASS_Deprecated) ? TEXT("The key class is deprecated.") : TEXT("A newer version of the key class exists.");
				return Fail(OutError, ValueFailure(TEXT("blackboard_key_type_invalid"), Reason,
					KeyPointer(Name) + (ClassPath.IsEmpty() ? TEXT("/candidate") : TEXT("/key_type_class")),
					MakeShared<FJsonValueString>(KeyClass->GetPathName()), MakeShared<FJsonValueString>(TEXT("concrete current nondeprecated BlackboardKeyType subclass"))));
			}

			FBlackboardEntry Entry;
			Entry.EntryName = EntryName;
#if WITH_EDITORONLY_DATA
			FString Description;
			Item->TryGetStringField(TEXT("description"), Description);
			Entry.EntryDescription = MoveTemp(Description);
			FString Category;
			Item->TryGetStringField(TEXT("category"), Category);
			Entry.EntryCategory = FName(*Category);
#endif
			bool bSynced = false;
			Item->TryGetBoolField(TEXT("instance_synced"), bSynced);
			Entry.bInstanceSynced = bSynced ? 1 : 0;
			const FBlackboardEntry* Existing = Blackboard.Keys.FindByPredicate(
				[&](const FBlackboardEntry& Key) { return Key.EntryName == EntryName; });
			Entry.KeyType = Existing && Existing->KeyType && Existing->KeyType->GetClass() == KeyClass
				? Existing->KeyType.Get() : NewObject<UBlackboardKeyType>(
					&Blackboard, KeyClass, NAME_None, RF_Transactional);
			const TSharedPtr<FJsonObject>* Properties = nullptr;
			if (Item->TryGetObjectField(TEXT("properties"), Properties))
			{
				UeAgentAuthoringObjectApply::FOptions Options;
				Options.bAllowReadOnly = true;
				Options.bShowDefaultsOnly = true;
				Options.bAllowDomainAuthorizedOwners = true;
				Options.bCompactValues = true;
				Options.bEmitEditorNotifications = true;
				UeAgentAuthoringDirectApply::FResult Result;
				if (!UeAgentDetailsAuthoringService::ApplyObjectProperties(Entry.KeyType, *Properties, Options, Result)) { OutError = Result.Diagnostic; return false; }
			}
			Desired.Add(MoveTemp(Entry));
		}

		Blackboard.Modify();
		Blackboard.Parent = Parent;
		Blackboard.Keys = MoveTemp(Desired);
		Blackboard.UpdateParentKeys();
		Blackboard.UpdateKeyIDs();
		Blackboard.UpdateIfHasSynchronizedKeys();
		Blackboard.PropagateKeyChangesToDerivedBlackboardAssets();
#if WITH_EDITOR
		UBlackboardData::OnBlackboardDataChanged.Broadcast(&Blackboard);
#endif
		return Blackboard.IsValid();
	}

	bool BuildDocumentFromSemanticModel(
		UBlackboardData& Blackboard,
		const TSharedRef<const UE::UAI::HjsonWorkspace::FAuthoringModel>& Model,
		const UE::UAI::Composition::HjsonImport::FPreparedAuthoringImport& Prepared,
		const UE::UAI::Composition::HjsonImport::FSemanticWorkspaceChangeProjection& Projection,
		TSharedPtr<FJsonObject>& OutDocument,
		UE::UAI::HjsonWorkspace::FWorkspaceError& Error)
	{
		using namespace UE::UAI;
		OutDocument.Reset();
		Error.Reset();
		auto FailWorkspace = [&](UE::UAI::Contract::FDiagnostic Diagnostic, const HjsonWorkspace::FSemanticUnit* Unit = nullptr) {
			TArray<Contract::FDiagnostic*> Pending{&Diagnostic};
			while (!Pending.IsEmpty())
			{
				auto& Issue = *Pending.Pop(EAllowShrinking::No);
				if (Unit && Issue.SourceKind != TEXT("native_result") && Issue.SourceKind != TEXT("command_input")
					&& Issue.Source.SourceUnit.IsEmpty())
				{
					Issue.Source.SourceUnit = Unit->GetUnitId();
					if (Issue.Source.InternalJsonPath.IsEmpty())
						Issue.Source.InternalJsonPath = !Issue.SemanticPointer.IsEmpty() ? Issue.SemanticPointer : Issue.Source.JsonPath;
				}
				for (auto& Child : Issue.Diagnostics) Pending.Add(&Child);
				if (Issue.PublicCause)
				{
					TSharedPtr<Contract::FDiagnostic> Cause = MakeShared<Contract::FDiagnostic>(*Issue.PublicCause);
					Issue.PublicCause = Cause;
					Pending.Add(Cause.Get());
				}
			}
			Error.SetFailure(MoveTemp(Diagnostic)); return false;
		};
		auto ModelFailure = [&](const TCHAR* Code, const TCHAR* Reason, const FString& Path,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected, const FString& InternalIdentity = FString()) {
			auto Diagnostic = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Path, Expected, Actual, TEXT("document_restore"), Reason);
			Diagnostic.SourceKind = TEXT("native_result");
			Diagnostic.bRetrySafe = false;
			Diagnostic.InternalContext = InternalIdentity;
			return FailWorkspace(MoveTemp(Diagnostic));
		};
		UE::UAI::Contract::FDiagnostic Detail;
		TSharedPtr<FJsonValue> Captured;
		if (!ReadDocument(Blackboard, Captured, Detail)) { Error.SetFailure(Detail); return false; }
		const HjsonWorkspace::FSemanticUnit* Root = nullptr;
		for (const auto& Unit : Model->GetUnits())
		{
			if (Unit.GetSemanticTypeRef() == DocumentSemanticId)
			{
				if (Root) return ModelFailure(TEXT("blackboard_semantic_document_ambiguous"), TEXT("The semantic model contains more than one blackboard document unit."),
					TEXT("/document_units"), MakeShared<FJsonValueString>(TEXT("at least 2")), MakeShared<FJsonValueNumber>(1));
				Root = &Unit;
			}
		}
		if (!Root) return ModelFailure(TEXT("blackboard_semantic_document_missing"), TEXT("The semantic model has no blackboard document unit."),
			TEXT("/document_units"), MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueNumber>(1));
		const auto* CanonicalKeys = Root->GetValues().Find(TEXT("blackboard_keys"));
		TSharedPtr<FJsonValue> KeysValue;
		if (!CanonicalKeys)
			return FailWorkspace(ValueFailure(TEXT("blackboard_semantic_keys_invalid"), TEXT("The blackboard document unit has no blackboard_keys value."),
				TEXT("/blackboard_keys"), nullptr, MakeShared<FJsonValueString>(TEXT("object"))), Root);
		if (!StateSemantic::CanonicalJson::TryToJsonValue(CanonicalKeys->Get(), KeysValue, Detail))
		{
			UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(Detail, TEXT("/blackboard_keys"));
			return FailWorkspace(Detail, Root);
		}
		if (!KeysValue.IsValid() || KeysValue->Type != EJson::Object)
			return FailWorkspace(ValueFailure(TEXT("blackboard_semantic_keys_invalid"), TEXT("The blackboard_keys value must be an object."),
				TEXT("/blackboard_keys"), JsonType(KeysValue), MakeShared<FJsonValueString>(TEXT("object"))), Root);

		const auto Desired = MakeShared<FJsonObject>();
		TMap<FString, FString> NewIds;
		TSet<FString> NewTypeKeys;
		int32 NewIndex = Blackboard.Keys.Num();
		for (const auto& Pair : KeysValue->AsObject()->Values)
		{
			if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::Object)
				return FailWorkspace(ValueFailure(TEXT("blackboard_semantic_key_invalid"), TEXT("The blackboard key definition must be an object."),
					KeyPointer(Pair.Key), JsonType(Pair.Value), MakeShared<FJsonValueString>(TEXT("object"))), Root);
			const auto Source = Pair.Value->AsObject();
			FString Operation;
			Source->TryGetStringField(TEXT("operation"), Operation);
			if (Operation == TEXT("remove")) continue;
			FString NativeName;
			if (!Source->TryGetStringField(TEXT("name"), NativeName)) NativeName = Pair.Key;
			const auto Previous = Captured->AsObject()->GetObjectField(TEXT("blackboard_keys"));
			const TSharedPtr<FJsonObject>* Existing = nullptr;
			TSharedPtr<FJsonObject> Item = Previous->TryGetObjectField(NativeName, Existing)
				? UeAgentAuthoringState::CloneCanonicalValue(MakeShared<FJsonValueObject>(*Existing))->AsObject()
				: MakeShared<FJsonObject>();
			const bool bNewType = Operation == TEXT("create") || Source->HasField(TEXT("candidate"));
			if (bNewType)
			{
				NewTypeKeys.Add(NativeName);
				if (!BuildNewKey(Blackboard, NativeName, Source, Item, Detail,
					Prepared.GetWorkspace()->FindValueNode(Root->GetUnitId(), TEXT("/blackboard_keys/")
						+ HjsonWorkspace::EscapeJsonPointerSegment(Pair.Key) + TEXT("/categories"))))
				{
					UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(Detail, KeyPointer(Pair.Key));
					return FailWorkspace(Detail, Root);
				}
				if (Operation == TEXT("create")) Item->SetNumberField(TEXT("index"), NewIndex++);
			}
			else if (Source->HasField(TEXT("categories")))
				return FailWorkspace(ValueFailure(TEXT("blackboard_initial_categories_require_new_type"), TEXT("Initial categories require creation or a candidate that changes the key type."),
					KeyPointer(Pair.Key) + TEXT("/categories"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueString>(TEXT("create operation or candidate binding"))), Root);
			FString TypeLabel;
			if (Existing && Source->TryGetStringField(TEXT("key_type"), TypeLabel)
				&& TypeLabel != (*Existing)->GetStringField(TEXT("key_type")))
				return FailWorkspace(ValueFailure(TEXT("blackboard_key_type_label_read_only_use_candidate"), TEXT("The key_type label is read-only; select a candidate to change the key type."),
					KeyPointer(Pair.Key) + TEXT("/key_type"), MakeShared<FJsonValueString>(TypeLabel), MakeShared<FJsonValueString>((*Existing)->GetStringField(TEXT("key_type")))), Root);
			for (const auto& Field : Source->Values)
			{
				if (Field.Key != TEXT("operation") && Field.Key != TEXT("unit_id")
					&& Field.Key != TEXT("categories")
					&& !(bNewType && (Field.Key == TEXT("key_type") || Field.Key == TEXT("key_type_class") || Field.Key == TEXT("properties"))))
					Item->SetField(Field.Key, UeAgentAuthoringState::CloneCanonicalValue(Field.Value));
			}
			Item->SetStringField(TEXT("name"), NativeName);
			if (bNewType) NewIds.Add(Pair.Key, NativeName);
			Desired->SetObjectField(NativeName, Item);
		}

		TMap<FString, TSet<FString>> EditedPaths;
		for (const auto& Pair : Projection.PropertyChangesByUnitId)
		{
			const auto* Unit = Prepared.GetWorkspace()->GetModel()->GetUnits().FindByPredicate(
				[&](const auto& Candidate) { return Candidate.GetUnitId() == Pair.Key; });
			if (!Unit) return ModelFailure(TEXT("blackboard_property_unit_missing"), TEXT("A projected property change refers to a semantic unit absent from the prepared workspace."),
				TEXT("/property_change/unit"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Pair.Key);
			const auto* Change = Prepared.GetChangeSetDraft().GetChanges().FindByPredicate(
				[&](const auto& Candidate)
				{
					return Unit->GetTargetIdentityKey() == Candidate.GetIdentity().GetCanonicalKey()
						|| Unit->GetTargetIdentityKey() == Candidate.GetIdentity().GetSelector(
							HjsonWorkspace::CanonicalAuthoringTargetIdentitySelector);
				});
			if (!Change) return ModelFailure(TEXT("blackboard_property_change_missing"), TEXT("The projected property unit has no matching change in the prepared change set."),
				TEXT("/property_change"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Unit->GetTargetIdentityKey());
			if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::Object)
				return ModelFailure(TEXT("blackboard_property_change_invalid"), TEXT("The projected property change is not an object."),
					TEXT("/property_change"), JsonType(Pair.Value), MakeShared<FJsonValueString>(TEXT("object")), Pair.Key);
			const FString NativeKey = Change->GetIdentity().GetSelector(TEXT("key_name"));
			const FString Path = Change->GetIdentity().GetSelector(TEXT("property_path"));
			const auto FailProperty = [&](const FString& Code, const FString& Reason, const FString& Expected) {
				auto Diagnostic = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, NativeKey + TEXT(".") + Path,
					MakeShared<FJsonValueString>(Expected), MakeShared<FJsonValueString>(Path), TEXT("document_restore"), Reason);
				UeAgentAuthoringDiagnostics::LocateAuthoringSource(Diagnostic, *Pair.Value->AsObject());
				return FailWorkspace(MoveTemp(Diagnostic), Unit);
			};
			EditedPaths.FindOrAdd(NativeKey).Add(Path);
			const TSharedPtr<FJsonObject>* Item = nullptr;
			if (!Desired->TryGetObjectField(NativeKey, Item))
				return FailProperty(TEXT("blackboard_property_owner_missing"), TEXT("The property change targets a key absent from the desired blackboard."), TEXT("existing key owner"));
			const TSharedPtr<FJsonObject>* Properties = nullptr;
			if (!(*Item)->TryGetObjectField(TEXT("properties"), Properties))
				return FailProperty(TEXT("blackboard_properties_missing"), TEXT("The target key has no captured property record."), TEXT("key property record"));
			bool bFound = false;
			for (const auto& Descriptor : (*Properties)->GetArrayField(TEXT("properties")))
			{
				if (Descriptor->AsObject()->GetStringField(TEXT("property_path")) == Path)
				{
					const auto Value = Pair.Value->AsObject()->TryGetField(TEXT("value"));
					if (!Value.IsValid()) return FailProperty(TEXT("blackboard_property_value_missing"), TEXT("The projected property change has no value."), TEXT("property value"));
					Descriptor->AsObject()->SetField(TEXT("value"), UeAgentAuthoringState::CloneCanonicalValue(Value));
					bFound = true;
					break;
				}
			}
			if (!bFound) return FailProperty(TEXT("blackboard_property_descriptor_missing"), TEXT("The edited property is absent from the captured key descriptors."), TEXT("property descriptor in the captured key"));
		}
		// Forward edits carry only authored changes. Replaying an unchanged sibling
		// after a native notification can overwrite the state that notification derived.
		for (const auto& Pair : EditedPaths)
		{
			if (NewTypeKeys.Contains(Pair.Key)) continue;
			const auto Properties = Desired->GetObjectField(Pair.Key)->GetObjectField(TEXT("properties"));
			auto Descriptors = Properties->GetArrayField(TEXT("properties"));
			Descriptors.RemoveAll([&](const auto& Descriptor) {
				return !Pair.Value.Contains(Descriptor->AsObject()->GetStringField(TEXT("property_path")));
			});
			Properties->SetArrayField(TEXT("properties"), Descriptors);
		}

		if (!HjsonWorkspace::TryRecordKeyedCreationArguments(TEXT("blackboard_keys"), KeysValue, Desired, NewIds, Detail)) { Error.SetFailure(Detail); return false; }
		OutDocument = MakeShared<FJsonObject>();
		const auto* Parent = Root->GetValues().Find(TEXT("parent"));
		OutDocument->SetStringField(TEXT("parent"), Parent ? (*Parent)->GetText() : FString());
		OutDocument->SetObjectField(TEXT("blackboard_keys"), Desired);
		return true;
	}
}

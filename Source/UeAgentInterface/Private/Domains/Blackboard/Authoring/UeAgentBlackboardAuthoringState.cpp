#include "Domains/Blackboard/Authoring/UeAgentBlackboardAuthoringState.h"

#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringDescriptors.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "UObject/Class.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticRegistry.h"

namespace UeAgentBlackboardAuthoring
{
	namespace
	{
		constexpr const TCHAR* AccessorId = TEXT("blackboard.structure.state");
		constexpr const TCHAR* DocumentKind = TEXT("blackboard.document");
		constexpr const TCHAR* KeyKind = TEXT("blackboard.key");
		constexpr const TCHAR* PropertyKind = TEXT("blackboard.key_property");
		FString IdentityModel(const FString& Kind) { return Kind + TEXT(".identity"); }

		UeAgentAuthoringState::EReadOutcome ReadState(
			const UeAgentAuthoringState::FReadRequest& Request,
			UeAgentAuthoringState::FStateValue& OutValue,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			using EOutcome = UeAgentAuthoringState::EReadOutcome;
			OutValue = {};
			OutError.Reset();
			const auto Failure = [&](const TCHAR* Code, const TCHAR* Reason, const FString& Path,
				TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
			{
				OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Path, Expected, Actual, TEXT("authoring_state"), Reason);
				OutError.SourceKind = TEXT("native_result");
				OutError.TargetPath = Request.TargetIdentity.GetSelector(TEXT("asset_path"));
				return EOutcome::Failed;
			};
			if (Request.SemanticId == KeyPropertySemanticId)
			{
				UeAgentAuthoringState::FStateAccessor PropertyAccessor;
				if (!UeAgentAuthoringState::FStateAccessorRegistry::Get().CopyAccessor(
						TEXT("core.property"), PropertyAccessor))
				{
					return Failure(TEXT("blackboard_property_accessor_missing"), TEXT("The property reader required to capture Blackboard keys is not registered."),
						TEXT("/property_reader"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueString>(TEXT("registered property reader")));
				}
				return PropertyAccessor(Request, OutValue, OutError);
			}

			const auto Value = MakeShared<FJsonObject>();
			if (Request.SemanticId == DocumentSemanticId)
			{
				const auto* Blackboard = Cast<UBlackboardData>(Request.RootObject);
				if (!Blackboard)
				{
					return Failure(TEXT("blackboard_document_binding_invalid"), TEXT("Blackboard document capture requires a BlackboardData object."),
						TEXT("/root_object"), Request.RootObject ? MakeShared<FJsonValueString>(Request.RootObject->GetClass()->GetPathName()) : TSharedPtr<FJsonValue>(),
						MakeShared<FJsonValueString>(UBlackboardData::StaticClass()->GetPathName()));
				}
				Value->SetStringField(TEXT("parent"), Blackboard->Parent
					? Blackboard->Parent->GetOutermost()->GetName() : FString());
#if WITH_EDITORONLY_DATA
				TArray<TSharedPtr<FJsonValue>> Inherited;
				for (const FBlackboardEntry& Entry : Blackboard->ParentKeys)
				{
					const auto Item = MakeShared<FJsonObject>();
					Item->SetStringField(TEXT("name"), Entry.EntryName.ToString());
					Item->SetStringField(TEXT("key_type"), Entry.KeyType
						? Entry.KeyType->GetClass()->GetDisplayNameText().ToString() : FString());
					Inherited.Add(MakeShared<FJsonValueObject>(Item));
				}
				Value->SetArrayField(TEXT("inherited_keys"), Inherited);
#endif
			}
			else if (Request.SemanticId == KeySemanticId)
			{
				const auto* Entry = static_cast<const FBlackboardEntry*>(Request.RootData);
				if (!Entry || !Entry->KeyType)
				{
					return Failure(TEXT("blackboard_key_binding_invalid"),
						!Entry ? TEXT("The Blackboard key capture has no native entry.") : TEXT("The Blackboard key has no native key type object."),
						TEXT("/blackboard_keys/") + Request.TargetIdentity.GetSelector(TEXT("key_name")).Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"))
							+ (Entry ? TEXT("/key_type") : TEXT("")), MakeShared<FJsonValueBoolean>(false),
						MakeShared<FJsonValueString>(Entry ? TEXT("native key type object") : TEXT("native Blackboard entry")));
				}
			// Blackboard keys use their native name as identity. Retain the same
			// binding used by mutation receipts before composition adds unit_id.
			Value->SetStringField(TEXT("id"), Request.TargetIdentity.GetSelector(TEXT("key_name")));
			Value->SetStringField(TEXT("name"), Entry->EntryName.ToString());
				const auto* Blackboard = CastChecked<UBlackboardData>(Request.RootObject);
				Value->SetNumberField(TEXT("index"), Blackboard->Keys.IndexOfByPredicate(
					[&](const auto& Key) { return Key.EntryName == Entry->EntryName; }));
				Value->SetStringField(TEXT("key_type"),
					Entry->KeyType->GetClass()->GetDisplayNameText().ToString());
				Value->SetStringField(TEXT("key_type_class"),
					Entry->KeyType->GetClass()->GetPathName());
#if WITH_EDITORONLY_DATA
				Value->SetStringField(TEXT("description"), Entry->EntryDescription);
				Value->SetStringField(TEXT("category"), Entry->EntryCategory.ToString());
#endif
				Value->SetBoolField(TEXT("instance_synced"), Entry->bInstanceSynced != 0);
			}
			else
			{
				return Failure(TEXT("blackboard_semantic_unsupported"), TEXT("The requested state semantic is not supported by the Blackboard reader."),
					TEXT("/semantic_id"), MakeShared<FJsonValueString>(Request.SemanticId), MakeShared<FJsonValueString>(TEXT("Blackboard document, key, or key-property semantic")));
			}
			OutValue.TargetIdentity = Request.TargetIdentity;
			OutValue.Type.Kind = TEXT("object");
			OutValue.Provenance = AccessorId;
			OutValue.Value = MakeShared<FJsonValueObject>(Value);
			OutValue.bAvailable = true;
			return EOutcome::Resolved;
		}
	}

	bool RegisterBlackboardState(UE::UAI::Contract::FDiagnostic& OutError)
	{
		struct FContract { const TCHAR* Semantic; const TCHAR* Kind; TArray<FString> Keys; };
		const TArray<FContract> Contracts = {
			{DocumentSemanticId, DocumentKind, {TEXT("asset_path")}},
			{KeySemanticId, KeyKind, {TEXT("asset_path"), TEXT("key_name")}},
			{KeyPropertySemanticId, PropertyKind,
				{TEXT("asset_path"), TEXT("key_name"), TEXT("property_path")}},
		};
		for (const FContract& Contract : Contracts)
		{
			UeAgentAuthoringIdentity::FModelDescriptor Identity;
			Identity.ModelId = IdentityModel(Contract.Kind);
			Identity.TargetKind = Contract.Kind;
			int32 Priority = 100;
			for (const FString& Key : Contract.Keys) Identity.Selectors.Add({Key, Priority++, false});
			Identity.KeyPatterns = {{TEXT("scoped"), 100, Contract.Keys}};
			UeAgentAuthoringIdentity::ERegistrationResult IdentityResult;
			if (!UeAgentAuthoringIdentity::FRegistry::Get().RegisterModel(
					Identity, IdentityResult, OutError)) return false;

			const bool bProperty = FString(Contract.Kind) == PropertyKind;
			UeAgentAuthoringSemanticRegistry::FSemanticDescriptor Semantic;
			Semantic.SemanticId = Contract.Semantic;
			Semantic.DomainId = UeAgentReflectionAuthoring::DomainId;
			Semantic.TargetKind = Contract.Kind;
			Semantic.ValueType = bProperty ? TEXT("*") : TEXT("object");
			Semantic.OwnerModel = TEXT("blackboard_data");
			Semantic.IdentityModel = Identity.ModelId;
			Semantic.StateAccessor = AccessorId;
			Semantic.WriteCapability = bProperty
				? UeAgentAuthoringSemanticRegistry::EWriteCapability::PropertyAssignment
				: UeAgentAuthoringSemanticRegistry::EWriteCapability::DomainMutation;
			Semantic.ConstraintSource = TEXT("blackboard.key_type");
			Semantic.AvailabilitySource = Semantic.StateAccessor;
			Semantic.Comparator = TEXT("uai.compare.canonical_value");
			UeAgentAuthoringSemanticRegistry::ERegistrationResult SemanticResult;
			if (!UeAgentAuthoringSemanticRegistry::FRegistry::Get().RegisterSemantic(
					Semantic, SemanticResult, OutError)) return false;
		}

		UeAgentAuthoringState::FStateAccessorDescriptor Accessor;
		Accessor.AccessorId = AccessorId;
		Accessor.TargetKinds = {DocumentKind, KeyKind, TEXT("property")};
		UeAgentAuthoringState::ERegistrationResult Result;
		return UeAgentAuthoringState::FStateAccessorRegistry::Get().RegisterAccessor(
			Accessor, ReadState, Result, OutError);
	}

	bool AppendStructuralReadRequests(
		const UBlackboardData& Blackboard,
		const FString& AssetPath,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		auto Add = [&](const TCHAR* Semantic, const TCHAR* Kind,
			const FString& KeyName, const UObject* RootObject, const void* RootData)
		{
			UeAgentAuthoringState::FReadRequest Request;
			Request.SemanticId = Semantic;
			TMap<FString, FString> Selectors{{TEXT("asset_path"), AssetPath}};
			if (!KeyName.IsEmpty()) Selectors.Add(TEXT("key_name"), KeyName);
			if (!UeAgentAuthoringIdentity::BuildCanonicalIdentity(
					IdentityModel(Kind), Kind, Selectors,
					Request.TargetIdentity, OutError)) return false;
			Request.TargetQuery.ModelId = Request.TargetIdentity.ModelId;
			Request.TargetQuery.TargetKind = Request.TargetIdentity.TargetKind;
			Request.TargetQuery.CanonicalKey = Request.TargetIdentity.CanonicalKey;
			Request.RootObject = RootObject;
			Request.RootData = RootData;
			OutRequests.Add(MoveTemp(Request));
			return true;
		};

		if (!Add(DocumentSemanticId, DocumentKind, {}, &Blackboard, nullptr)) return false;
		for (int32 KeyIndex = 0; KeyIndex < Blackboard.Keys.Num(); ++KeyIndex)
		{
			const FBlackboardEntry& Entry = Blackboard.Keys[KeyIndex];
			const FString KeyName = Entry.EntryName.ToString();
			if (KeyName.IsEmpty() || !Entry.KeyType)
			{
				OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("blackboard_local_key_invalid"),
					FString::Printf(TEXT("/keys/%d/%s"), KeyIndex, KeyName.IsEmpty() ? TEXT("name") : TEXT("key_type")),
					MakeShared<FJsonValueString>(KeyName.IsEmpty() ? TEXT("nonempty key name") : TEXT("native key type object")),
					KeyName.IsEmpty() ? MakeShared<FJsonValueString>(KeyName) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()),
					TEXT("authoring_state"), KeyName.IsEmpty() ? TEXT("A native Blackboard key has an empty name.") : TEXT("A native Blackboard key has no type object."));
				OutError.SourceKind = TEXT("native_result");
				OutError.TargetPath = AssetPath;
				return false;
			}
			if (!Add(KeySemanticId, KeyKind, KeyName, &Blackboard, &Entry)) return false;
			UeAgentAuthoringState::FEditablePropertyReadOptions Options;
			Options.SemanticId = KeyPropertySemanticId;
			Options.Limits.bShowDefaultsOnly = true;
			Options.BuildSnapshotIdentity = [AssetPath, KeyName](
				const FString& Path,
				UeAgentAuthoringIdentity::FCanonicalIdentity& Identity,
				UE::UAI::Contract::FDiagnostic& Error)
			{
				return UeAgentAuthoringIdentity::BuildCanonicalIdentity(
					IdentityModel(PropertyKind), PropertyKind,
					{{TEXT("asset_path"), AssetPath}, {TEXT("key_name"), KeyName},
						{TEXT("property_path"), Path}}, Identity, Error);
			};
			if (!UeAgentAuthoringState::AppendEditableObjectPropertyReadRequests(
					Entry.KeyType, Options, OutRequests, OutError)) return false;
		}
		return true;
	}
}

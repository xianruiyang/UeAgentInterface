#include "DomainAdapters/LocalizationStringTable/UeAgentStringTableAuthoringState.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Internationalization/StringTableCore.h"
#include "Internationalization/StringTableRegistry.h"
#include "Misc/PackageName.h"
#include "UObject/UObjectGlobals.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringEncodedTypes.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticRegistry.h"

namespace UE::UAI::DomainAdapters::LocalizationStringTable
{
	namespace
	{
		const FString IdentityModel = TEXT("string_table.authoring.identity");
		const FString TargetKind = TEXT("string_table");
		const FString AccessorId = TEXT("string_table.authoring.state");
		const FString TargetSemantic = TEXT("string_table.export.target");
		const FString EntrySemantic = TEXT("string_table.entries");
		const FString PresenceSemantic = TEXT("string_table.target.presence");

		TSharedRef<FJsonObject> CapturePresence(const FString& Path)
		{
			const auto Value = MakeShared<FJsonObject>();
			Value->SetBoolField(TEXT("object_present"), FindObject<UObject>(nullptr, *Path) != nullptr);
			Value->SetBoolField(TEXT("disk_present"), FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(Path)));
			Value->SetBoolField(TEXT("registry_present"), FStringTableRegistry::Get().FindStringTable(FName(*Path)).IsValid());
			return Value;
		}

		UeAgentAuthoringState::EReadOutcome ReadState(const UeAgentAuthoringState::FReadRequest& Request,
			UeAgentAuthoringState::FStateValue& OutValue, UE::UAI::Contract::FDiagnostic& OutError)
		{
			using namespace UeAgentAuthoringState;
			const FStringTable* Table = static_cast<const FStringTable*>(Request.RootData);
			if (!Table && Request.SemanticId != PresenceSemantic)
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("string_table_state_missing"),
					TEXT("The native StringTable required for state capture is unavailable."), TEXT("state_capture"));
				OutError.SourceKind = TEXT("native_result");
				OutError.TargetPath = Request.RootOwnerPath;
				OutError.PropertyPath = TEXT("/root_data_present");
				OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
				OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				OutError.ActualValuePresent = true;
				OutError.ExpectedValuePresent = true;
				return EReadOutcome::Failed;
			}
			OutValue = FStateValue();
			OutValue.TargetIdentity = Request.TargetIdentity;
			OutValue.Type.Kind = TEXT("object");
			OutValue.Provenance = AccessorId;
			TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
			if (Request.SemanticId == PresenceSemantic)
			{
				Value = CapturePresence(Request.RootOwnerPath);
			}
			else if (Request.SemanticId == TargetSemantic)
			{
				Value->SetStringField(TEXT("asset_path"), Request.RootOwnerPath);
				Value->SetStringField(TEXT("asset_kind"), TargetKind);
			}
			else if (Request.SemanticId == EntrySemantic)
			{
				Value = CaptureStringTableState(*Table);
				OutValue.bEditable = true;
			}
			else
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("string_table_semantic_unsupported"),
					TEXT("The StringTable state accessor does not support the requested semantic."), TEXT("state_capture"));
				OutError.SourceKind = TEXT("native_result");
				OutError.TargetPath = Request.RootOwnerPath;
				OutError.PropertyPath = TEXT("/semantic_id");
				OutError.ActualValue = MakeShared<FJsonValueString>(Request.SemanticId);
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TargetSemantic + TEXT(", ") + EntrySemantic + TEXT(" or ") + PresenceSemantic);
				OutError.ActualValuePresent = true;
				OutError.ExpectedValuePresent = true;
				return EReadOutcome::Failed;
			}
			OutValue.Value = MakeShared<FJsonValueObject>(Value);
			return EReadOutcome::Resolved;
		}
	}

	TSharedRef<FJsonObject> CaptureStringTableState(const FStringTable& Table)
	{
		const auto State = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Entries;
		State->SetStringField(TEXT("namespace"), Table.GetNamespace());
		Table.EnumerateSourceStrings([&](const FString& Key, const FString& Source)
		{
			const auto Entry = MakeShared<FJsonObject>();
			const auto Metadata = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("key"), Key);
			Entry->SetStringField(TEXT("source"), Source);
			Table.EnumerateMetaData(Key, [&](FName Name, const FString& Value)
			{
				Metadata->SetStringField(Name.ToString(), Value);
				return true;
			});
			UeAgentAuthoringEncodedTypes::FScope::RecordNameKeyedObject(Metadata);
			Entry->SetObjectField(TEXT("metadata"), Metadata);
			Entries.Add(MakeShared<FJsonValueObject>(Entry));
			return true;
		});
		Entries.Sort([](const auto& A, const auto& B) {
			return A->AsObject()->GetStringField(TEXT("key")).Compare(
				B->AsObject()->GetStringField(TEXT("key")), ESearchCase::CaseSensitive) < 0; });
		State->SetArrayField(TEXT("entries"), Entries);
		return State;
	}

	bool RegisterStringTableAuthoringState(UE::UAI::Contract::FDiagnostic& OutError)
	{
		UeAgentAuthoringIdentity::FModelDescriptor Identity;
		Identity.ModelId = IdentityModel;
		Identity.TargetKind = TargetKind;
		Identity.Selectors = {{TEXT("asset_path"), 100, false}};
		Identity.KeyPatterns = {{TEXT("asset"), 0, {TEXT("asset_path")}}};
		UeAgentAuthoringIdentity::ERegistrationResult IdentityResult;
		if (!UeAgentAuthoringIdentity::FRegistry::Get().RegisterModel(Identity, IdentityResult, OutError)) return false;
		using namespace UeAgentAuthoringSemanticRegistry;
		FDomainDescriptor Domain;
		Domain.DomainId = TEXT("string_table.authoring");
		Domain.TargetKinds = {TargetKind};
		Domain.MutationProviderId = TEXT("string_table.authoring.apply");
		Domain.CapabilityTags = {TEXT("table"), TEXT("editor_authoring")};
		ERegistrationResult DomainResult;
		if (!FRegistry::Get().RegisterDomain(Domain, DomainResult, OutError)) return false;
		for (const FString& SemanticId : {TargetSemantic, EntrySemantic, PresenceSemantic})
		{
			FSemanticDescriptor Descriptor;
			Descriptor.SemanticId = SemanticId;
			Descriptor.DomainId = Domain.DomainId;
			Descriptor.TargetKind = TargetKind;
			Descriptor.ValueType = TEXT("object");
			Descriptor.OwnerModel = TEXT("string_table_asset_owner");
			Descriptor.IdentityModel = IdentityModel;
			Descriptor.StateAccessor = AccessorId;
			Descriptor.WriteCapability = SemanticId != EntrySemantic
				? EWriteCapability::ReadOnly : EWriteCapability::DomainMutation;
			Descriptor.ConstraintSource = TEXT("string_table.native");
			Descriptor.AvailabilitySource = AccessorId;
			Descriptor.Comparator = TEXT("uai.compare.canonical_value");
			Descriptor.CapabilityTags = Domain.CapabilityTags;
			ERegistrationResult Result;
			if (!FRegistry::Get().RegisterSemantic(Descriptor, Result, OutError)) return false;
		}
		UeAgentAuthoringState::FStateAccessorDescriptor Accessor;
		Accessor.AccessorId = AccessorId;
		Accessor.TargetKinds = {TargetKind};
		Accessor.CapabilityTags = Domain.CapabilityTags;
		UeAgentAuthoringState::ERegistrationResult StateResult;
		return UeAgentAuthoringState::RegisterBuiltInStateAccessors(UeAgentAuthoringState::FStateAccessorRegistry::Get(), OutError)
			&& UeAgentAuthoringState::FStateAccessorRegistry::Get().RegisterAccessor(Accessor, &ReadState, StateResult, OutError);
	}

	bool IsStringTableTargetAbsent(const FString& AssetPath)
	{
		const auto Presence = CapturePresence(AssetPath);
		return !Presence->GetBoolField(TEXT("object_present")) && !Presence->GetBoolField(TEXT("disk_present"))
			&& !Presence->GetBoolField(TEXT("registry_present"));
	}

	bool BuildStringTablePresenceReadRequests(const FString& AssetPath,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests, UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutRequests.Reset();
		UeAgentAuthoringState::FReadRequest Request;
		Request.SemanticId = PresenceSemantic;
		Request.RootOwnerPath = AssetPath;
		if (!UeAgentAuthoringIdentity::BuildCanonicalIdentity(IdentityModel, TargetKind,
			{{TEXT("asset_path"), AssetPath}}, Request.TargetIdentity, OutError)) return false;
		Request.TargetQuery.ModelId = Request.TargetIdentity.ModelId;
		Request.TargetQuery.TargetKind = Request.TargetIdentity.TargetKind;
		Request.TargetQuery.CanonicalKey = Request.TargetIdentity.CanonicalKey;
		OutRequests.Add(MoveTemp(Request));
		return true;
	}

	bool BuildStringTableAuthoringReadRequests(const FStringTable& Table, const FString& AssetPath,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests, UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutRequests.Reset();
		for (const FString& SemanticId : {TargetSemantic, EntrySemantic})
		{
			UeAgentAuthoringState::FReadRequest Request;
			Request.SemanticId = SemanticId;
			if (SemanticId == TargetSemantic) Request.MarkExportTarget();
			Request.RootData = &Table;
			Request.RootOwnerPath = AssetPath;
			if (!UeAgentAuthoringIdentity::BuildCanonicalIdentity(IdentityModel, TargetKind,
				{{TEXT("asset_path"), AssetPath}}, Request.TargetIdentity, OutError)) return false;
			Request.TargetQuery.ModelId = Request.TargetIdentity.ModelId;
			Request.TargetQuery.TargetKind = Request.TargetIdentity.TargetKind;
			Request.TargetQuery.CanonicalKey = Request.TargetIdentity.CanonicalKey;
			OutRequests.Add(MoveTemp(Request));
		}
		return true;
	}
}

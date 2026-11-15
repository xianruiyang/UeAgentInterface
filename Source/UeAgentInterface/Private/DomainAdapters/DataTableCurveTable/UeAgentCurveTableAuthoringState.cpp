#include "DomainAdapters/DataTableCurveTable/UeAgentCurveTableAuthoringState.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Engine/CurveTable.h"
#include "Curves/SimpleCurve.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"
#include "Utilities/UeAgentCurveJson.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringEncodedTypes.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticRegistry.h"

namespace UE::UAI::DomainAdapters::DataTableCurveTable
{
	namespace
	{
		const FString IdentityModel = TEXT("curve_table.authoring.identity");
		const FString TargetKind = TEXT("curve_table");
		const FString AccessorId = TEXT("curve_table.authoring.state");
		const FString TargetSemantic = TEXT("curve_table.export.target");
		const FString RowsSemantic = TEXT("curve_table.rows");
		const FString PresenceSemantic = TEXT("curve_table.target.presence");

		TSharedRef<FJsonObject> CapturePresence(const FString& Path)
		{
			const auto Value = MakeShared<FJsonObject>();
			Value->SetBoolField(TEXT("object_present"), FindObject<UObject>(nullptr, *Path) != nullptr);
			Value->SetBoolField(TEXT("disk_present"), FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(Path)));
			Value->SetBoolField(TEXT("registry_present"), FAssetRegistryModule::GetRegistry().GetAssetByObjectPath(FSoftObjectPath(Path)).IsValid());
			return Value;
		}

		UeAgentAuthoringState::EReadOutcome ReadState(const UeAgentAuthoringState::FReadRequest& Request,
			UeAgentAuthoringState::FStateValue& OutValue, UE::UAI::Contract::FDiagnostic& OutError)
		{
			using namespace UeAgentAuthoringState;
			const UCurveTable* Table = Cast<UCurveTable>(Request.RootObject);
			if (!Table && Request.SemanticId != PresenceSemantic)
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("curve_table_target_missing"),
					TEXT("The state capture root is missing or is not a CurveTable."), TEXT("state_capture"));
				OutError.SourceKind = TEXT("native_result");
				OutError.TargetPath = Request.RootOwnerPath;
				OutError.PropertyPath = TEXT("/root_class");
				OutError.ActualValue = Request.RootObject
					? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Request.RootObject->GetClass()->GetPathName()))
					: TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>());
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("/Script/Engine.CurveTable"));
				return EReadOutcome::Failed;
			}
			OutValue = FStateValue();
			OutValue.TargetIdentity = Request.TargetIdentity;
			OutValue.Type.Kind = TEXT("object");
			OutValue.Provenance = AccessorId;
			TSharedPtr<FJsonObject> Value;
			if (Request.SemanticId == PresenceSemantic) Value = CapturePresence(Request.RootOwnerPath);
			else if (Request.SemanticId == RowsSemantic)
			{
				if (!CaptureCurveTableRows(*Table, Value, OutError)) return EReadOutcome::Failed;
				OutValue.bEditable = true;
			}
			else if (Request.SemanticId == TargetSemantic)
			{
				Value = MakeShared<FJsonObject>();
				Value->SetStringField(TEXT("asset_path"), Request.RootOwnerPath);
				Value->SetStringField(TEXT("asset_kind"), TargetKind);
			}
			else
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("curve_table_semantic_unsupported"),
					TEXT("The CurveTable state accessor does not support the requested semantic."), TEXT("state_capture"));
				OutError.SourceKind = TEXT("native_result");
				OutError.TargetPath = Request.RootOwnerPath;
				OutError.PropertyPath = TEXT("/semantic_supported");
				OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
				OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				OutError.ActualEvidence = MakeShared<FJsonValueString>(Request.SemanticId);
				OutError.ExpectedEvidence = MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
					MakeShared<FJsonValueString>(TargetSemantic), MakeShared<FJsonValueString>(RowsSemantic), MakeShared<FJsonValueString>(PresenceSemantic)});
				return EReadOutcome::Failed;
			}
			OutValue.Value = MakeShared<FJsonValueObject>(Value);
			return EReadOutcome::Resolved;
		}
	}

	bool CaptureCurveTableRows(const UCurveTable& Table, TSharedPtr<FJsonObject>& OutValue, UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutValue.Reset();
		const ECurveTableMode Mode = Table.GetCurveTableMode();
		const bool bSimple = Mode == ECurveTableMode::SimpleCurves;
		const auto Value = MakeShared<FJsonObject>();
		Value->SetStringField(TEXT("table_curve_mode"), Mode == ECurveTableMode::Empty ? TEXT("empty") : bSimple ? TEXT("simple") : TEXT("rich"));
		Value->SetObjectField(TEXT("row_schema"), Mode == ECurveTableMode::Empty
			? UeAgentCurveJson::BuildEmptyCurveTableRowSchema() : UeAgentCurveJson::BuildCurveTableRowSchema(bSimple));
		const auto Rows = MakeShared<FJsonObject>();
		auto Add = [&](FName Name, const TSharedPtr<FJsonObject>& Curve)
		{
			const auto Property = MakeShared<FJsonObject>();
			Property->SetStringField(TEXT("type"), TEXT("curve"));
			Property->SetObjectField(TEXT("value"), Curve);
			const auto Properties = MakeShared<FJsonObject>();
			Properties->SetObjectField(TEXT("Curve"), Property);
			const auto Row = MakeShared<FJsonObject>();
			UeAgentAuthoringEncodedTypes::FScope::SetNameField(Row, TEXT("name"), Name);
			Row->SetObjectField(TEXT("properties"), Properties);
			Rows->SetObjectField(Name.ToString(), Row);
		};
		if (bSimple)
			for (const auto& Pair : Table.GetSimpleCurveRowMap())
				Add(Pair.Key, UeAgentCurveJson::BuildSimpleCurveJson(*Pair.Value, {}, {}, true, {}));
		else if (Mode == ECurveTableMode::RichCurves)
			for (const auto& Pair : Table.GetRichCurveRowMap())
				Add(Pair.Key, UeAgentCurveJson::BuildFloatCurveJson(*Pair.Value, {}, {}, true, {}));
		UeAgentAuthoringEncodedTypes::FScope::RecordNameKeyedObject(Rows);
		Value->SetObjectField(TEXT("rows"), Rows);
		OutValue = Value;
		return true;
	}

	bool RegisterCurveTableAuthoringState(UE::UAI::Contract::FDiagnostic& OutError)
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
		Domain.DomainId = TEXT("curve_table.authoring");
		Domain.TargetKinds = {TargetKind};
		Domain.MutationProviderId = TEXT("curve_table.authoring.apply");
		Domain.CapabilityTags = {TEXT("table"), TEXT("editor_authoring")};
		ERegistrationResult Registered;
		if (!FRegistry::Get().RegisterDomain(Domain, Registered, OutError)) return false;
		for (const FString& Id : {TargetSemantic, RowsSemantic, PresenceSemantic})
		{
			FSemanticDescriptor Descriptor;
			Descriptor.SemanticId = Id;
			Descriptor.DomainId = Domain.DomainId;
			Descriptor.TargetKind = TargetKind;
			Descriptor.ValueType = TEXT("object");
			Descriptor.OwnerModel = TEXT("curve_table_asset_owner");
			Descriptor.IdentityModel = IdentityModel;
			Descriptor.StateAccessor = AccessorId;
			Descriptor.WriteCapability = Id == RowsSemantic ? EWriteCapability::DomainMutation : EWriteCapability::ReadOnly;
			Descriptor.ConstraintSource = TEXT("curve_table.native_mode");
			Descriptor.AvailabilitySource = AccessorId;
			Descriptor.Comparator = TEXT("uai.compare.canonical_value");
			Descriptor.CapabilityTags = Domain.CapabilityTags;
			if (!FRegistry::Get().RegisterSemantic(Descriptor, Registered, OutError)) return false;
		}
		UeAgentAuthoringState::FStateAccessorDescriptor Accessor;
		Accessor.AccessorId = AccessorId;
		Accessor.TargetKinds = {TargetKind};
		Accessor.CapabilityTags = Domain.CapabilityTags;
		UeAgentAuthoringState::ERegistrationResult StateResult;
		return UeAgentAuthoringState::RegisterBuiltInStateAccessors(UeAgentAuthoringState::FStateAccessorRegistry::Get(), OutError)
			&& UeAgentAuthoringState::FStateAccessorRegistry::Get().RegisterAccessor(Accessor, &ReadState, StateResult, OutError);
	}

	bool BuildCurveTableDomainReadRequests(const UCurveTable& Table, const FString& Path,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests, UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutRequests.Reset();
		for (const FString& Id : {TargetSemantic, RowsSemantic})
		{
			UeAgentAuthoringState::FReadRequest Request;
			Request.SemanticId = Id;
			if (Id == TargetSemantic) Request.MarkExportTarget();
			Request.RootObject = &Table;
			Request.RootOwnerPath = Path;
			if (!UeAgentAuthoringIdentity::BuildCanonicalIdentity(IdentityModel, TargetKind,
				{{TEXT("asset_path"), Path}}, Request.TargetIdentity, OutError)) return false;
			Request.TargetQuery.ModelId = Request.TargetIdentity.ModelId;
			Request.TargetQuery.TargetKind = Request.TargetIdentity.TargetKind;
			Request.TargetQuery.CanonicalKey = Request.TargetIdentity.CanonicalKey;
			OutRequests.Add(MoveTemp(Request));
		}
		return true;
	}

	bool IsCurveTableTargetAbsent(const FString& Path)
	{
		const auto Presence = CapturePresence(Path);
		return !Presence->GetBoolField(TEXT("object_present")) && !Presence->GetBoolField(TEXT("disk_present"))
			&& !Presence->GetBoolField(TEXT("registry_present"));
	}

	bool BuildCurveTablePresenceReadRequests(const FString& Path,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests, UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutRequests.Reset();
		UeAgentAuthoringState::FReadRequest Request;
		Request.SemanticId = PresenceSemantic;
		Request.RootOwnerPath = Path;
		if (!UeAgentAuthoringIdentity::BuildCanonicalIdentity(IdentityModel, TargetKind,
			{{TEXT("asset_path"), Path}}, Request.TargetIdentity, OutError)) return false;
		Request.TargetQuery.ModelId = Request.TargetIdentity.ModelId;
		Request.TargetQuery.TargetKind = Request.TargetIdentity.TargetKind;
		Request.TargetQuery.CanonicalKey = Request.TargetIdentity.CanonicalKey;
		OutRequests.Add(MoveTemp(Request));
		return true;
	}

	bool BuildCurveTableAuthoringReadRequests(const UCurveTable& Table, const FString& Path,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests, UE::UAI::Contract::FDiagnostic& OutError)
	{
		return BuildCurveTableDomainReadRequests(Table, Path, OutRequests, OutError)
			&& UeAgentAuthoringState::AppendEditableObjectPropertyReadRequests(&Table,
				UeAgentAuthoringState::FEditablePropertyReadOptions(), OutRequests, OutError);
	}
}

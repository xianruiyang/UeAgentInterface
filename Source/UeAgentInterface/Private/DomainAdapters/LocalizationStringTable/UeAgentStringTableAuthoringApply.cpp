#include "DomainAdapters/LocalizationStringTable/UeAgentStringTableAuthoringApply.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

#include "DomainAdapters/LocalizationStringTable/UeAgentStringTableAuthoringState.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Core/StateSemantic/UeAgentExactStringKeys.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "Internationalization/StringTableRegistry.h"
#include "UObject/Package.h"
#include "UObject/WeakObjectPtr.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringAssetWorkspaceApply.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceLoader.h"

namespace UE::UAI::DomainAdapters::LocalizationStringTable
{
	namespace
	{
		using namespace UeAgentAuthoringOperationApply;
		using namespace UeAgentApplyInputContract;
		const FString ProviderId = TEXT("string_table.authoring.apply");
		const FString InputId = TEXT("uai.apply_input.string_table");
		const FString WorkspaceId = TEXT("uai.apply_workspace.string_table");

		bool Fail(FOperationFailure& Failure, const UE::UAI::Contract::FDiagnostic& Diagnostic)
		{ Failure.Diagnostic = Diagnostic; return false; }

		void CopyTable(FStringTable& Target, const FStringTable& Source)
		{
			Target.SetNamespace(Source.GetNamespace());
			Target.ClearSourceStrings();
			Source.EnumerateSourceStrings([&](const FString& Key, const FString& Text)
			{
				Target.SetSourceString(Key, Text);
				Source.EnumerateMetaData(Key, [&](FName Name, const FString& Value)
				{
					Target.SetMetaData(Key, Name, Value);
					return true;
				});
				return true;
			});
		}

		FString PointerKey(const FString& Key)
		{ return Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")); }

		TSharedPtr<FJsonValue> JsonFact(const TSharedPtr<FJsonValue>& Value)
		{
			if (!Value.IsValid()) return nullptr;
			if (Value->Type == EJson::String || Value->Type == EJson::Number
				|| Value->Type == EJson::Boolean || Value->Type == EJson::Null) return Value;
			return MakeShared<FJsonValueString>(Value->Type == EJson::Array ? TEXT("array") : TEXT("object"));
		}

		UE::UAI::Contract::FDiagnostic TableFailure(const FString& Code, const FString& Reason,
			const FString& Target, const FString& Field, const TSharedPtr<FJsonValue>& Actual,
			const TSharedPtr<FJsonValue>& Expected, const FJsonObject* Input = nullptr)
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("authoring_apply"));
			Error.TargetPath = Target;
			Error.PropertyPath = Field;
			Error.ActualValue = Actual;
			Error.ExpectedValue = Expected;
			Error.ActualValuePresent = Actual.IsValid();
			Error.ExpectedValuePresent = Expected.IsValid();
			Error.SourceKind = Input ? (Actual.IsValid() ? TEXT("authored_value") : TEXT("related_input")) : TEXT("native_result");
			if (Input)
			{
				Error.Source.JsonPath = Field;
				UE::UAI::Contract::FSourceLocation Source;
				if (UeAgentAuthoringDiagnostics::FScopedSourceLocations::Locate(*Input, FString(), Source, Field))
					Error.Source = MoveTemp(Source);
			}
			return Error;
		}

		UE::UAI::Contract::FDiagnostic TableConditionFailure(const FString& Code, const FString& Reason,
			const FString& Target, const FString& Field, bool Actual, bool Expected = true)
		{
			return TableFailure(Code, Reason, Target, Field, MakeShared<FJsonValueBoolean>(Actual), MakeShared<FJsonValueBoolean>(Expected));
		}

		bool PrepareTable(const FJsonObject& Desired, const FString& Path, FStringTable& Proposed, FOperationFailure& Failure)
		{
			FString Namespace;
			const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
			if (!Desired.HasTypedField<EJson::String>(TEXT("namespace"))
				|| !Desired.TryGetStringField(TEXT("namespace"), Namespace))
				return Fail(Failure, TableFailure(TEXT("string_table_state_invalid"),
					TEXT("The StringTable namespace must be present as a string."), Path, TEXT("/namespace"),
					JsonFact(Desired.TryGetField(TEXT("namespace"))), MakeShared<FJsonValueString>(TEXT("string")), &Desired));
			if (!Desired.TryGetArrayField(TEXT("entries"), Entries))
				return Fail(Failure, TableFailure(TEXT("string_table_state_invalid"),
					TEXT("The StringTable entries must be present as an array."), Path, TEXT("/entries"),
					JsonFact(Desired.TryGetField(TEXT("entries"))), MakeShared<FJsonValueString>(TEXT("array")), &Desired));
			for (const auto& Field : Desired.Values)
				if (Field.Key != TEXT("namespace") && Field.Key != TEXT("entries"))
					return Fail(Failure, TableFailure(TEXT("string_table_field_unknown"),
						TEXT("The StringTable state contains an unsupported field."), Path, TEXT("/") + PointerKey(Field.Key),
						MakeShared<FJsonValueString>(Field.Key), MakeShared<FJsonValueString>(TEXT("namespace or entries")), &Desired));
			Proposed.SetNamespace(Namespace);
			TSet<FString, UE::UAI::StateSemantic::FExactStringKeyFuncs> Keys;
			for (int32 EntryIndex = 0; EntryIndex < Entries->Num(); ++EntryIndex)
			{
				const auto& Entry = (*Entries)[EntryIndex];
				const FString EntryPath = TEXT("/entries/") + FString::FromInt(EntryIndex);
				if (!Entry.IsValid() || Entry->Type != EJson::Object)
					return Fail(Failure, TableFailure(TEXT("string_table_entry_invalid"),
						TEXT("Each StringTable entry must be an object."), Path, EntryPath,
						JsonFact(Entry), MakeShared<FJsonValueString>(TEXT("object")), &Desired));
				const auto Record = Entry->AsObject();
				FString Key;
				if (!Record->HasTypedField<EJson::String>(TEXT("key")) || !Record->TryGetStringField(TEXT("key"), Key))
					return Fail(Failure, TableFailure(TEXT("string_table_entry_key_invalid"),
						TEXT("The entry key must be present as a string."), Path, EntryPath + TEXT("/key"),
						JsonFact(Record->TryGetField(TEXT("key"))), MakeShared<FJsonValueString>(TEXT("nonempty unique string")), &Desired));
				if (Key.IsEmpty() || Keys.Contains(Key))
					return Fail(Failure, TableFailure(TEXT("string_table_entry_key_invalid"),
						Key.IsEmpty() ? TEXT("The entry key must not be empty.") : TEXT("The entry key duplicates an earlier entry."),
						Path, EntryPath + TEXT("/key"), MakeShared<FJsonValueString>(Key),
						MakeShared<FJsonValueString>(TEXT("nonempty unique string")), &Desired));
				Keys.Add(Key);
				FString Source;
				if (!Record->HasTypedField<EJson::String>(TEXT("source")) || !Record->TryGetStringField(TEXT("source"), Source))
					return Fail(Failure, TableFailure(TEXT("string_table_source_invalid"),
						TEXT("The entry source must be present as a string."), Path, EntryPath + TEXT("/source"),
						JsonFact(Record->TryGetField(TEXT("source"))), MakeShared<FJsonValueString>(TEXT("string")), &Desired));
				for (const auto& Field : Record->Values)
					if (Field.Key != TEXT("key") && Field.Key != TEXT("source") && Field.Key != TEXT("metadata"))
						return Fail(Failure, TableFailure(TEXT("string_table_entry_field_unknown"),
							TEXT("The StringTable entry contains an unsupported field."), Path, EntryPath + TEXT("/") + PointerKey(Field.Key),
							MakeShared<FJsonValueString>(Field.Key), MakeShared<FJsonValueString>(TEXT("key, source or metadata")), &Desired));
				Proposed.SetSourceString(Key, Source);
				if (!Record->HasField(TEXT("metadata"))) continue;
				const TSharedPtr<FJsonObject>* Metadata = nullptr;
				if (!Record->TryGetObjectField(TEXT("metadata"), Metadata))
					return Fail(Failure, TableFailure(TEXT("string_table_metadata_invalid"),
						TEXT("The entry metadata must be an object."), Path, EntryPath + TEXT("/metadata"),
						JsonFact(Record->TryGetField(TEXT("metadata"))), MakeShared<FJsonValueString>(TEXT("object")), &Desired));
				TSet<FName> Names;
				for (const auto& Field : (*Metadata)->Values)
				{
					const FName Name(*Field.Key);
					FString Value;
					if (Names.Contains(Name))
						return Fail(Failure, TableFailure(TEXT("string_table_metadata_invalid"),
							TEXT("The metadata name duplicates an earlier name under Unreal name comparison."),
							Path, EntryPath + TEXT("/metadata/") + PointerKey(Field.Key),
							MakeShared<FJsonValueString>(Field.Key), MakeShared<FJsonValueString>(TEXT("unique metadata name")), &Desired));
					if (!Field.Value.IsValid() || Field.Value->Type != EJson::String || !Field.Value->TryGetString(Value))
						return Fail(Failure, TableFailure(TEXT("string_table_metadata_invalid"),
							TEXT("The metadata value must be a string."), Path, EntryPath + TEXT("/metadata/") + PointerKey(Field.Key),
							JsonFact(Field.Value), MakeShared<FJsonValueString>(TEXT("string")), &Desired));
					Names.Add(Name);
					Proposed.SetMetaData(Key, Name, Value);
				}
			}
			return true;
		}

		class FTableState final : public IPreparedOperationState
		{
		public:
			FTableState(FString InPath, UStringTable* InActual, FStringTableRef InProposed)
				: Path(MoveTemp(InPath)), Actual(InActual), Proposed(InProposed), bCreate(InActual == nullptr) {}
			FString GetTargetKey() const override { return Path; }
			EOperation GetResolvedOperation() const override { return bCreate ? EOperation::Create : EOperation::Update; }
			void CollectRollbackObjects(TArray<UObject*>& Objects) const override
			{
				if (Actual) { Objects.AddUnique(Actual); Objects.AddUnique(Actual->GetOutermost()); }
			}
			bool CaptureRollback(FOperationFailure& Failure) override
			{
				if (bCreate) return IsStringTableTargetAbsent(Path) || Fail(Failure, TableConditionFailure(TEXT("string_table_create_target_exists"), TEXT("The StringTable create target already exists."), Path, TEXT("/target_present"), true, false));
				if (!Actual) return Fail(Failure, TableConditionFailure(TEXT("string_table_target_missing"), TEXT("The native StringTable target is no longer available."), Path, TEXT("/target_present"), false));
				Before = FStringTable::NewStringTable();
				CopyTable(*Before, *Actual->GetStringTable());
				return true;
			}
			bool ApplyProposed(FOperationFailure&) override { return true; }
			bool BuildExpectedReadRequests(TArray<UeAgentAuthoringState::FReadRequest>& Requests,
				FOperationFailure& Failure) const override { return Read(Proposed.Get(), Requests, Failure); }
			bool BuildActualReadRequests(TArray<UeAgentAuthoringState::FReadRequest>& Requests,
				FOperationFailure& Failure) const override
			{
				return Read(Actual ? &Actual->GetStringTable().Get() : nullptr, Requests, Failure);
			}
			bool BuildBeforeReadRequests(TArray<UeAgentAuthoringState::FReadRequest>& Requests,
				FOperationFailure& Failure) const override
			{
				if (!bCreate) return BuildActualReadRequests(Requests, Failure);
				UE::UAI::Contract::FDiagnostic Error;
				return BuildStringTablePresenceReadRequests(Path, Requests, Error)
					|| Fail(Failure, Error);
			}
			bool ApplyLifecycle(FOperationFailure& Failure) override
			{
				if (bCreate)
				{
					if (!IsStringTableTargetAbsent(Path)) return Fail(Failure, TableConditionFailure(TEXT("string_table_create_target_exists"), TEXT("The StringTable create target already exists."), Path, TEXT("/target_present"), true, false));
					const FString PackageName = FPackageName::ObjectPathToPackageName(Path);
					UPackage* Package = CreatePackage(*PackageName);
					Actual = NewObject<UStringTable>(Package, *FPackageName::GetLongPackageAssetName(PackageName), RF_Public | RF_Standalone | RF_Transactional);
					if (!Actual) return Fail(Failure, TableConditionFailure(TEXT("string_table_create_failed"), TEXT("Unreal did not create the StringTable object."), Path, TEXT("/target_present"), false));
					bCreated = true;
					FAssetRegistryModule::AssetCreated(Actual);
				}
				if (!Actual || !Proposed) return Fail(Failure, TableConditionFailure(TEXT("string_table_target_missing"), !Actual ? TEXT("The native StringTable target is unavailable.") : TEXT("The prepared StringTable state is unavailable."), Path, !Actual ? TEXT("/target_present") : TEXT("/prepared_state_present"), false));
				Actual->Modify();
				CopyTable(*Actual->GetMutableStringTable(), *Proposed);
				Actual->MarkPackageDirty();
				return true;
			}
			bool Compile(FOperationFailure&) override { return true; }
			bool Save(FOperationFailure&) override { return true; }
			bool BuildDurablePackagePlans(TArray<FPackageDurablePlan>& Plans, FOperationFailure& Failure) const override
			{
				if (!Actual && !bCreate) return Fail(Failure, TableConditionFailure(TEXT("string_table_target_missing"), TEXT("The StringTable target required for the durable save is unavailable."), Path, TEXT("/target_present"), false));
				if (!BuildPackageDurablePlan(Path, FPackageName::ObjectPathToPackageName(Path), Actual,
					EPackageDurableIntent::Save, Plans, Failure, true,
					bCreate ? EPackageBeforePresence::Absent : EPackageBeforePresence::Present)) return false;
				Plans.Last().ReadbackPolicy = EPackageReadbackPolicy::ReplaceResidentPackage;
				return true;
			}
			bool Rollback(FOperationFailure& Failure) override
			{
				if (bCreate) return RemoveCreated(Failure);
				if (!Actual || !Before) return Fail(Failure, TableConditionFailure(TEXT("string_table_restore_state_missing"), !Actual ? TEXT("The StringTable target required for rollback is unavailable.") : TEXT("The captured StringTable rollback state is unavailable."), Path, !Actual ? TEXT("/target_present") : TEXT("/rollback_state_present"), false));
				CopyTable(*Actual->GetMutableStringTable(), *Before);
				return true;
			}
			bool SupportsPersistentReadback() const override { return true; }
			bool PrepareCheckpointRestore(FOperationFailure& Failure) override
			{
				return bCreate ? RemoveCreated(Failure) : ReleaseForPersistentReadback(Failure);
			}
			bool RebindAfterCheckpointRestore(FOperationFailure& Failure) override
			{
				return bCreate ? (IsStringTableTargetAbsent(Path) || Fail(Failure, TableConditionFailure(TEXT("string_table_absent_restore_unconfirmed"), TEXT("The StringTable target is still present after restoring its absent state."), Path, TEXT("/target_present"), true, false)))
					: RebindAfterPersistentReadback(Failure);
			}
			bool ReleaseForPersistentReadback(FOperationFailure&) override
			{
				if (Actual) ReleasedObjects.Add(FWeakObjectPtr(Actual));
				Actual = nullptr;
				Proposed.Reset();
				Before.Reset();
				return true;
			}
			bool RebindAfterPersistentReadback(FOperationFailure& Failure) override
			{
				Actual = LoadObject<UStringTable>(nullptr, *Path);
				if (!Actual) return Fail(Failure, TableConditionFailure(TEXT("string_table_reload_failed"), TEXT("The saved StringTable could not be reloaded."), Path, TEXT("/target_present"), false));
				// A replaced UStringTable unregisters its cached ID in FinishDestroy.
				// Do not restore that ID until the old UObject is gone, including garbage objects.
				for (const FWeakObjectPtr& ReleasedObject : ReleasedObjects)
				{
					if (ReleasedObject.IsValid(true, true))
						return Fail(Failure, TableConditionFailure(TEXT("string_table_replaced_object_retained"), TEXT("A replaced StringTable object is still alive during persistent readback or cleanup."), Path, TEXT("/replaced_object_alive"), true, false));
				}
				ReleasedObjects.Reset();
				auto& Registry = FStringTableRegistry::Get();
				const auto Native = Actual->GetMutableStringTable();
				const auto Registered = Registry.FindStringTable(Actual->GetStringTableId());
				if (Registered.IsValid() && Registered != Native)
					return Fail(Failure, TableConditionFailure(TEXT("string_table_registry_binding_conflict"), TEXT("The StringTable registry points to a different native table."), Path, TEXT("/registry_matches_target"), false));
				if (!Registered.IsValid()) Registry.RegisterStringTable(Actual->GetStringTableId(), Native);
				return Registry.FindStringTable(Actual->GetStringTableId()) == Native
					|| Fail(Failure, TableConditionFailure(TEXT("string_table_registry_rebind_failed"), TEXT("The StringTable registry did not retain the reloaded native table."), Path, TEXT("/registry_matches_target"), false));
			}
		private:
			bool RemoveCreated(FOperationFailure& Failure)
			{
				if (!bCreated) return IsStringTableTargetAbsent(Path) || Fail(Failure, TableConditionFailure(TEXT("string_table_absent_restore_unconfirmed"), TEXT("The StringTable target is still present after restoring its absent state."), Path, TEXT("/target_present"), true, false));
				UStringTable* Created = Actual ? Actual : FindObject<UStringTable>(nullptr, *Path);
				Actual = nullptr;
				Proposed.Reset();
				Before.Reset();
				if (Created)
				{
					const FWeakObjectPtr Witness(Created);
					const int32 Deleted = ObjectTools::ForceDeleteObjects({Created}, false);
					if (Deleted != 1) return Fail(Failure, TableFailure(TEXT("string_table_created_object_delete_failed"),
						TEXT("Unreal did not report deleting exactly one created StringTable."), Path, TEXT("/deleted_object_count"),
						MakeShared<FJsonValueNumber>(Deleted), MakeShared<FJsonValueNumber>(1)));
					if (Witness.IsValid(true, true)) return Fail(Failure, TableConditionFailure(TEXT("string_table_created_object_delete_failed"),
						TEXT("The created StringTable object is still alive after deletion."), Path, TEXT("/created_object_alive"), true, false));
				}
				for (const auto& Released : ReleasedObjects)
					if (Released.IsValid(true, true)) return Fail(Failure, TableConditionFailure(TEXT("string_table_replaced_object_retained"), TEXT("A replaced StringTable object is still alive during persistent readback or cleanup."), Path, TEXT("/replaced_object_alive"), true, false));
				ReleasedObjects.Reset();
				bCreated = false;
				return IsStringTableTargetAbsent(Path) || Fail(Failure, TableConditionFailure(TEXT("string_table_absent_restore_unconfirmed"), TEXT("The StringTable target is still present after restoring its absent state."), Path, TEXT("/target_present"), true, false));
			}
			bool Read(const FStringTable* Table, TArray<UeAgentAuthoringState::FReadRequest>& Requests,
				FOperationFailure& Failure) const
			{
				UE::UAI::Contract::FDiagnostic Error;
				if (!Table) return Fail(Failure, TableConditionFailure(TEXT("string_table_state_missing"),
					TEXT("The native StringTable state required for readback is unavailable."), Path, TEXT("/state_present"), false));
				return BuildStringTableAuthoringReadRequests(*Table, Path, Requests, Error) || Fail(Failure, Error);
			}
			FString Path;
			UStringTable* Actual = nullptr;
			TArray<FWeakObjectPtr> ReleasedObjects;
			FStringTablePtr Proposed;
			FStringTablePtr Before;
			bool bCreate = false;
			bool bCreated = false;
		};

		class FProvider final : public IApplyProvider
		{
		public:
			FString GetProviderId() const override { return ProviderId; }
			bool PrepareOperation(const FPrepareRequest& Request,
				TUniquePtr<IPreparedOperationState>& OutState, FOperationFailure& Failure) const override
			{
				if (!Request.IsValid())
					return Fail(Failure, TableConditionFailure(TEXT("string_table_operation_invalid"),
						!Request.Contract ? TEXT("The internal StringTable prepare request has no contract.")
							: !Request.Unit ? TEXT("The internal StringTable prepare request has no workspace unit.")
							: TEXT("The internal StringTable prepare request has no operation input."),
						FString(), !Request.Contract ? TEXT("/prepare_request/contract_present")
							: !Request.Unit ? TEXT("/prepare_request/unit_present") : TEXT("/prepare_request/input_present"), false));
				if (Request.Input->RequestedOperation != EOperation::Update && Request.Input->RequestedOperation != EOperation::Create)
					return Fail(Failure, TableFailure(TEXT("string_table_operation_invalid"),
						TEXT("StringTable authoring supports only create or update."), FString(), TEXT("/operation"),
						MakeShared<FJsonValueNumber>(static_cast<int32>(Request.Input->RequestedOperation)),
						MakeShared<FJsonValueString>(TEXT("create or update"))));
				FString Path;
				if (!Request.Input->Target.IsValid() || !Request.Input->Target->TryGetStringField(TEXT("asset_path"), Path))
					return Fail(Failure, TableFailure(TEXT("string_table_apply_document_invalid"),
						TEXT("The prepared StringTable target must contain an asset_path string."), Path, TEXT("/target/asset_path"),
						Request.Input->Target ? JsonFact(Request.Input->Target->TryGetField(TEXT("asset_path"))) : nullptr,
						MakeShared<FJsonValueString>(TEXT("asset path string"))));
				if (!Request.Input->ProviderExtension.IsValid())
					return Fail(Failure, TableConditionFailure(TEXT("string_table_apply_document_invalid"),
						TEXT("The prepared StringTable state document is unavailable."), Path, TEXT("/provider_extension_present"), false));
				const bool bCreate = Request.Input->RequestedOperation == EOperation::Create;
				if (bCreate)
				{
					const FString PackageName = FPackageName::ObjectPathToPackageName(Path);
					if (!FPackageName::IsValidLongPackageName(PackageName)
						|| !Path.Equals(HjsonWorkspace::DomainAuthoring::ObjectPathFromPackageIdentity(PackageName), ESearchCase::CaseSensitive))
						return Fail(Failure, TableFailure(TEXT("string_table_create_target_invalid"),
							TEXT("The create target must be the canonical object path of a valid long package name."),
							Path, TEXT("/target/asset_path"), MakeShared<FJsonValueString>(Path),
							MakeShared<FJsonValueString>(TEXT("/Game/Folder/Asset.Asset"))));
				}
				if (bCreate && !IsStringTableTargetAbsent(Path)) return Fail(Failure, TableConditionFailure(TEXT("string_table_create_target_exists"),
					TEXT("The StringTable create target already exists."), Path, TEXT("/target_present"), true, false));
				UStringTable* Actual = bCreate ? nullptr : LoadObject<UStringTable>(nullptr, *Path);
				if (!Actual && !bCreate) return Fail(Failure, TableConditionFailure(TEXT("string_table_target_missing"),
					TEXT("The StringTable update target could not be loaded."), Path, TEXT("/target_present"), false));
				const auto Proposed = FStringTable::NewStringTable();
				if (!PrepareTable(*Request.Input->ProviderExtension, Path, *Proposed, Failure)) return false;
				OutState = MakeUnique<FTableState>(Path, Actual, Proposed);
				return true;
			}
		};
		FContract MakeInputContract()
		{
			FContract Result;
			Result.Schema = TEXT("uai.apply_input_contract.v1");
			Result.ContractId = InputId;
			Result.ContractVersion = TEXT("1.0.0");
			Result.ProfileId = TEXT("string_table");
			Result.TargetKind = TEXT("string_table");
			Result.SemanticDescriptorRefs = {TEXT("string_table.entries"), TEXT("string_table.export.target")};
			Result.ReflectionBaseline.RootKinds = {TEXT("uobject"), TEXT("ustruct")};
			Result.ReflectionBaseline.RecursiveNodes = {TEXT("scalar"), TEXT("struct"), TEXT("owned_object"),
				TEXT("reference"), TEXT("array"), TEXT("set"), TEXT("map"), TEXT("fixed_array"), TEXT("optional")};
			Result.ReflectionBaseline.UnknownFieldPolicy = TEXT("reject");
			Result.ReflectionBaseline.ExternalReferencePolicy = TEXT("replace_reference_only");
			Result.ReflectionBaseline.OwnedObjectPolicy = TEXT("create_or_locate_with_real_outer_then_recurse");
			Result.PropertyPayload.Mode = TEXT("reflection");
			Result.PropertyPayload.PropertyPathField = TEXT("property_path");
			Result.PropertyPayload.ValueField = TEXT("value");
			Result.PropertyPayload.OmittedPatchFieldPolicy = TEXT("preserve_current");
			Result.InputFields = {
				{TEXT("asset_path"), TEXT("identity"), TEXT("ue_asset_path"), {EOperation::Update, EOperation::Create}},
				{TEXT("operation"), TEXT("operation_control"), TEXT("apply_operation"), {EOperation::Update, EOperation::Create}},
				{TEXT("entries"), TEXT("provider_extension"), TEXT("array")},
				{TEXT("namespace"), TEXT("provider_extension"), TEXT("string")}};
			FOperationDescriptor Operation;
			Operation.Operation = EOperation::Update;
			Operation.IdentityOneOf = {{TEXT("asset_path")}};
			Operation.AcceptedSections = {TEXT("target"), TEXT("provider_extension")};
			Operation.OmittedFieldPolicy = TEXT("preserve_current");
			Operation.NativeDefaultPolicy = TEXT("forbidden");
			Result.Operations = {Operation};
			Operation.Operation = EOperation::Create;
			Result.Operations.Add(Operation);
			Result.ProviderId = ProviderId;
			Result.UnknownInputFieldPolicy = TEXT("reject");
			Result.Verification.StateSource = TEXT("shared_state_snapshot");
			Result.Verification.CompareSource = TEXT("shared_semantic_compare");
			return Result;
		}

		UeAgentApplyWorkspaceContract::FContract MakeWorkspaceContract()
		{
			using namespace UeAgentApplyWorkspaceContract;
			UeAgentApplyWorkspaceContract::FContract Result;
			Result.Schema = TEXT("uai.apply_workspace_contract.v1");
			Result.ContractId = WorkspaceId;
			Result.ContractVersion = TEXT("1.0.0");
			Result.ProfileId = TEXT("string_table");
			Result.Sources = {
				{ESourceKind::Directory, TEXT("physical"), TEXT("index")},
				{ESourceKind::SingleFile, TEXT("virtual"), TEXT("object")},
				{ESourceKind::Inline, TEXT("virtual"), TEXT("object")}};
			Result.Entry.DirectoryEntry = TEXT("index.hjson");
			Result.Entry.VirtualUnitId = TEXT("table");
			FUnitDescriptor Unit;
			Unit.UnitId = TEXT("table");
			Unit.PathPattern = TEXT("index.hjson");
			Unit.Role = TEXT("object");
			Unit.Cardinality = TEXT("one");
			Unit.bRequired = true;
			Unit.RouteId = TEXT("string_table");
			Unit.ApplyContractRef = InputId;
			Unit.MergePolicy = TEXT("replace");
			Unit.AbsentSemantics = TEXT("error");
			Unit.EmptySemantics = TEXT("error");
			Result.Units = {Unit};
			Result.ReferencePolicy.Base = TEXT("workspace_root");
			Result.ReferencePolicy.RootContainment = TEXT("required");
			Result.ReferencePolicy.ExternalReferences = TEXT("reject");
			Result.ReferencePolicy.CaseCollision = TEXT("reject");
			Result.DependencyPolicy.EdgeSource = TEXT("contract_and_references");
			Result.DependencyPolicy.CyclePolicy = TEXT("reject");
			Result.DependencyPolicy.UnresolvedPolicy = TEXT("reject");
			Result.DependencyPolicy.Order = TEXT("stable_topological");
			Result.Preflight.Scope = TEXT("whole_workspace");
			Result.Preflight.MutationBeforeSuccess = TEXT("forbid");
			Result.Preflight.bDiagnosticsIncludeSource = true;
			Result.Transaction.Mode = TEXT("atomic");
			Result.Transaction.RollbackOn = {TEXT("mutation_failed"), TEXT("compile_failed"),
				TEXT("save_failed"), TEXT("verification_failed")};
			Result.Execution.Compile = TEXT("provider_hook");
			Result.Execution.Save = TEXT("caller_controlled");
			Result.Execution.Verification = TEXT("shared_snapshot_required");
			Result.UnknownFilePolicy = TEXT("reject");
			Result.DuplicateTargetPolicy = TEXT("reject");
			return Result;
		}
	}

	bool ApplyStringTableAuthoring(const FString& Target, const FString& RequestId,
		const TSharedPtr<FJsonObject>& Desired, HjsonWorkspace::DomainAuthoring::FRootPropertyApplyOutcome& Outcome,
		const UeAgentAuthoringOperationApply::FBackendOptions* BackendOptions, bool bCreate)
	{
		using namespace UeAgentAuthoringOperationApply;
		UE::UAI::Contract::FDiagnostic Error;
		UeAgentApplyInputContract::ERegistrationResult Registration;
		UeAgentAuthoringOperationApply::ERegistrationResult ProviderRegistration;
		FProviderRegistry Providers;
		if (!RegisterStringTableAuthoringState(Error)
			|| !UeAgentApplyInputContract::FRegistry::Get().RegisterContract(MakeInputContract(), Registration, Error)
			|| !Providers.RegisterProvider(MakeShared<FProvider>(), ProviderRegistration, Error))
		{
			Outcome.Diagnostic = Error;
			return false;
		}
		const auto Contract = MakeWorkspaceContract();
		const auto TargetObject = MakeShared<FJsonObject>();
		TargetObject->SetStringField(TEXT("asset_path"), Target);
		const auto Document = MakeShared<FJsonObject>();
		Document->SetStringField(TEXT("operation"), bCreate ? TEXT("create") : TEXT("update"));
		Document->SetObjectField(TEXT("target"), TargetObject);
		Document->SetObjectField(TEXT("provider_extension"), Desired);
		UeAgentAuthoringWorkspaceLoader::FLoadResult Load;
		if (!UeAgentAuthoringWorkspaceLoader::LoadWorkspace(Contract,
			UeAgentAuthoringWorkspaceLoader::FSourceRequest::MakeInlineObject(Document, TEXT("inline://string-table")), Load))
		{
			Outcome.Diagnostic = Load.Diagnostic;
			return false;
		}
		FBackendOptions Options = BackendOptions ? *BackendOptions : FBackendOptions();
		Options.FinalReadback = Outcome.FinalReadback;
		Options.TransactionDescription = NSLOCTEXT("UeAgentInterface", "StringTableAuthoring", "Edit StringTable");
		FApplyWorkspaceResult Result;
		UeAgentAuthoringAssetWorkspaceApply::FFailure Failure;
		const bool bApplied = UeAgentAuthoringAssetWorkspaceApply::Execute(Contract,
			Load.Workspace, Providers, RequestId, true, Options, Result, Failure);
		Outcome.bApplied = bApplied;
		Outcome.Phase = Failure.Phase;
		Outcome.Diagnostic = Failure.Diagnostic;
		if (!bApplied && Result.Execution.bRollbackAttempted)
		{
			Outcome.Diagnostic.RollbackAttempted = true;
			Outcome.Diagnostic.RollbackSucceeded = Result.Execution.bRollbackSucceeded;
			Outcome.Diagnostic.TransactionStatus = Result.Execution.Status;
			if (!Result.Execution.bRollbackSucceeded) Outcome.Diagnostic.bRetrySafe = false;
		}
		return bApplied;
	}
}

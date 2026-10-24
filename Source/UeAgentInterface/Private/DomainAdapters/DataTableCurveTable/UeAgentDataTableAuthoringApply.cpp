#include "DomainAdapters/DataTableCurveTable/UeAgentDataTableAuthoringApply.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "DomainAdapters/DataTableCurveTable/UeAgentDataTableAuthoringState.h"
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectStructuredAssetMechanics.h"
#include "Engine/DataTable.h"
#include "DataTableEditorUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "ObjectTools.h"
#include "Misc/PackageName.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/StructOnScope.h"
#include "UObject/Package.h"
#include "Utilities/AuthoringObjectApply/UeAgentAuthoringObjectApply.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringObjectApplyProvider.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringAssetWorkspaceApply.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceLoader.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceRouter.h"

namespace UE::UAI::DomainAdapters::DataTableCurveTable
{
	namespace
	{
		using namespace UeAgentAuthoringOperationApply;
		using namespace UeAgentApplyInputContract;
		const FString ProviderId = TEXT("data_table.authoring.apply");
		const FString InputId = TEXT("uai.apply_input.data_table");
		const FString WorkspaceId = TEXT("uai.apply_workspace.data_table");

		bool Fail(FOperationFailure& Failure, const UE::UAI::Contract::FDiagnostic& Diagnostic)
		{ Failure.Diagnostic = Diagnostic; return false; }

		UE::UAI::Contract::FDiagnostic DataInputFailure(const FString& Code, const FString& Reason,
			const FString& Target, const FString& Field, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
		{
			auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Field, Expected, Actual, TEXT("authoring_apply"), Reason);
			Error.TargetPath = Target;
			return Error;
		}

		UE::UAI::Contract::FDiagnostic DataLifecycleFailure(const FString& Code, const FString& Reason,
			const FString& Target, const FString& Field, bool Actual, bool Expected)
		{
			auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Field,
				MakeShared<FJsonValueBoolean>(Expected), MakeShared<FJsonValueBoolean>(Actual), TEXT("authoring_apply"), Reason);
			Error.TargetPath = Target;
			Error.SourceKind = TEXT("native_result");
			Error.Source.JsonPath.Reset();
			Error.SemanticPointer.Reset();
			return Error;
		}

		bool ReplaceRows(UDataTable& Target, const UDataTable& Source, Contract::FDiagnostic& OutError)
		{
			TMap<FName, const uint8*> Rows;
			for (const auto& Pair : Source.GetRowMap()) Rows.Add(Pair.Key, Pair.Value);
			return AssetDataSimpleEffect::TryReplaceDataTableRows(Target, Source.RowStruct.Get(), Rows, OutError);
		}

		struct FRowIntent
		{
			FName Name;
			TSharedPtr<FStructOnScope> Storage;
			UeAgentAuthoringObjectApply::FPreparedApply Prepared;
		};

		void BindDataTableRowSource(UE::UAI::Contract::FDiagnostic& Diagnostic,
			const UeAgentAuthoringWorkspaceRouter::FWorkspaceUnit& Unit, const FString& RowPath,
			const FJsonObject& Descriptor)
		{
			if (Diagnostic.SourceKind == TEXT("native_result") || Diagnostic.SourceKind == TEXT("command_input")) return;
			if (!Diagnostic.Source.SourceFile.IsEmpty() && !Diagnostic.Source.SourceFile.StartsWith(TEXT("inline://"))
				&& !Diagnostic.Source.SourceFile.StartsWith(TEXT("resolved:"))) return;
			FString Property;
			if (!Descriptor.TryGetStringField(TEXT("property_path"), Property)) return;
			const FString Base = RowPath + TEXT("/properties/")
				+ Property.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
			TFunction<void(UE::UAI::Contract::FDiagnostic&)> Bind = [&](auto& Issue)
			{
				for (auto& Child : Issue.Diagnostics) Bind(Child);
				if (Issue.PublicCause)
				{
					auto Cause = MakeShared<UE::UAI::Contract::FDiagnostic>(*Issue.PublicCause);
					Bind(*Cause); Issue.PublicCause = MoveTemp(Cause);
				}
				if (Issue.SourceKind == TEXT("native_result") || Issue.SourceKind == TEXT("command_input")) return;
				if (!Issue.Source.SourceFile.IsEmpty() && !Issue.Source.SourceFile.StartsWith(TEXT("inline://"))
					&& !Issue.Source.SourceFile.StartsWith(TEXT("resolved:"))) return;
				// Collected failures may already belong to an earlier descriptor.
				if (Issue.SemanticPointer.StartsWith(TEXT("/rows/")) || Issue.Source.InternalJsonPath.StartsWith(TEXT("/rows/"))) return;
				const FString Relative = !Issue.Source.InternalJsonPath.IsEmpty() ? Issue.Source.InternalJsonPath
					: !Issue.SemanticPointer.IsEmpty() ? Issue.SemanticPointer : Issue.Source.JsonPath;
				FString Pointer = Base;
				if (Relative == TEXT("/property_path")) { /* The generated identifier came from this actual property key. */ }
				else if (!Relative.IsEmpty() && Relative != TEXT("/"))
				{
					const FString Suffix = Relative.StartsWith(TEXT("/")) ? Relative
						: TEXT("/") + Relative.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
					Pointer += Suffix == TEXT("/value") || Suffix.StartsWith(TEXT("/value/"))
						? Suffix : TEXT("/value") + Suffix;
				}
				const auto Source = Unit.ResolveDiagnosticSource(TEXT("/provider_extension") + Pointer);
				if (!Source.SourceFile.IsEmpty() && !Source.SourceFile.StartsWith(TEXT("inline://"))
					&& !Source.SourceFile.StartsWith(TEXT("resolved:"))) Issue.Source = Source;
				else
				{
					Issue.Source.SourceFile.Reset();
					Issue.Source.Line = 0; Issue.Source.Column = 0;
					Issue.Source.JsonPath = Pointer;
					Issue.Source.InternalJsonPath = Pointer;
				}
				Issue.SemanticPointer = Pointer;
				if (Issue.SourceKind.IsEmpty()) Issue.SourceKind = TEXT("authored_value");
			};
			Bind(Diagnostic);
		}

		bool VerifyRows(UDataTable& Target, TArray<FRowIntent>& Intents, FOperationFailure& Failure, const FString& LogicalTarget)
		{
			for (FRowIntent& Intent : Intents)
			{
				uint8* const* Row = Target.GetRowMap().Find(Intent.Name);
				if (!Row) {
					auto Warning = DataLifecycleFailure(TEXT("data_table_intent_row_missing"),
						TEXT("Native row replacement returned no row for this prepared row intent."), LogicalTarget,
						TEXT("/prepared_row_present"), false, true);
					Warning.CandidateName = Intent.Name.ToString();
					Warning.Stage = TEXT("data_table_observation");
					UeAgentAuthoringDiagnostics::ReportConsistencyWarning(Warning);
					continue;
				}
				UeAgentAuthoringObjectApply::FApplyResult Result;
				if (!UeAgentAuthoringObjectApply::VerifyPreparedStruct(Intent.Prepared,
					Target.RowStruct.Get(), *Row, &Target, Result))
				{
					Failure.Diagnostic = MoveTemp(Result.Diagnostic);
					const FString RowPath = TEXT("/rows/") + Intent.Name.ToString().Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
					Failure.Diagnostic.PropertyPath = RowPath + (Failure.Diagnostic.PropertyPath.IsEmpty()
						? FString() : TEXT("/properties/") + Failure.Diagnostic.PropertyPath);
					UeAgentAuthoringDiagnostics::ReportConsistencyWarning(Failure.Diagnostic);
					Failure = FOperationFailure();
				}
			}
			return true;
		}

		bool ApplyRows(UDataTable& Target, const FJsonObject& Rows, const UDataTable& Baseline,
			FOperationFailure& Failure, TArray<FRowIntent>* OutIntents = nullptr,
			const TSharedPtr<const UeAgentAuthoringWorkspaceRouter::FWorkspaceUnit>& SourceUnit = nullptr,
			const FString& LogicalTarget = FString())
		{
			TMap<FName, TSharedPtr<FStructOnScope>> Storage;
			TMap<FName, const uint8*> Views;
			TArray<FRowIntent> Intents;
			const auto Escape = [](const FString& Key) { return Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")); };
			const auto LocateInput = [&](UE::UAI::Contract::FDiagnostic& Error, const FString& Pointer)
			{
				if (!SourceUnit.IsValid()) { Error.SourceKind = TEXT("native_result"); return; }
				const auto Source = SourceUnit->ResolveDiagnosticSource(TEXT("/provider_extension") + Pointer);
				if (!Source.SourceFile.IsEmpty() && !Source.SourceFile.StartsWith(TEXT("inline://"))
					&& !Source.SourceFile.StartsWith(TEXT("resolved:"))) Error.Source = Source;
				// Otherwise the outer domain resolver still owns this actual relative input pointer.
				Error.SemanticPointer = Pointer;
			};
			const auto Invalid = [&](const FString& Code, const FString& Reason, const FString& Path, const FString& Expected,
				const TSharedPtr<FJsonValue>& Actual) {
				Failure.Diagnostic = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Path,
					MakeShared<FJsonValueString>(Expected), Actual, TEXT("authoring_apply"), Reason);
				Failure.Diagnostic.TargetPath = LogicalTarget;
				LocateInput(Failure.Diagnostic, Path);
				return Fail(Failure, Failure.Diagnostic);
			};
			for (const auto& Pair : Rows.Values)
			{
				const FString RowPath = TEXT("/rows/") + Escape(Pair.Key);
				const FName Name(*Pair.Key);
				if (Name.IsNone() || Storage.Contains(Name) || !Pair.Value.IsValid() || Pair.Value->Type != EJson::Object)
				{
					if (Name.IsNone() || Storage.Contains(Name))
					{
						Failure.Diagnostic = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("data_table_row_name_invalid"), RowPath,
							MakeShared<FJsonValueString>(TEXT("a unique non-None row name")), MakeShared<FJsonValueString>(Pair.Key),
							TEXT("authoring_apply"), Name.IsNone() ? TEXT("The DataTable row key resolves to the reserved None name.")
								: TEXT("Two DataTable row keys resolve to the same case-insensitive native name."));
						Failure.Diagnostic.TargetPath = LogicalTarget;
						LocateInput(Failure.Diagnostic, RowPath);
						return false;
					}
					return Invalid(TEXT("data_table_row_name_invalid"), TEXT("Each DataTable row must be an object."), RowPath, TEXT("object"), Pair.Value);
				}
				const TSharedPtr<FJsonObject>* Properties = nullptr;
				if (!Pair.Value->AsObject()->TryGetObjectField(TEXT("properties"), Properties))
					return Invalid(TEXT("data_table_row_properties_missing"), TEXT("The DataTable row requires a properties object."), RowPath + TEXT("/properties"),
						TEXT("object"), Pair.Value->AsObject()->TryGetField(TEXT("properties")));
				const auto Data = MakeShared<FStructOnScope>(Target.GetRowStruct());
				if (const uint8* const* Row = Baseline.GetRowMap().Find(Name))
					Target.GetRowStruct()->CopyScriptStruct(Data->GetStructMemory(), *Row);
				TArray<TSharedPtr<FJsonValue>> Descriptors;
				for (const auto& Property : (*Properties)->Values)
				{
					const FString PropertyPath = RowPath + TEXT("/properties/") + Escape(Property.Key);
					if (!Property.Value.IsValid() || Property.Value->Type != EJson::Object)
						return Invalid(TEXT("data_table_property_invalid"), TEXT("Each authored DataTable property must be an object containing value."), PropertyPath, TEXT("object containing value"), Property.Value);
					const auto Value = Property.Value->AsObject()->TryGetField(TEXT("value"));
					if (!Value.IsValid()) return Invalid(TEXT("data_table_property_value_missing"), TEXT("The authored DataTable property has no value field."), PropertyPath + TEXT("/value"), TEXT("value"), nullptr);
					const auto Descriptor = MakeShared<FJsonObject>();
					Descriptor->SetStringField(TEXT("property_path"), Property.Key);
					Descriptor->SetField(TEXT("value"), Value);
					Descriptors.Add(MakeShared<FJsonValueObject>(Descriptor));
				}
				UeAgentAuthoringObjectApply::FOptions Options;
				Options.bEmitEditorNotifications = false;
				Options.bMarkPackageDirty = false;
				Options.DiagnosticTargetPath = LogicalTarget;
				if (SourceUnit.IsValid())
				{
					const UStruct* SourceStruct = Target.RowStruct.Get();
					const void* SourceData = Data->GetStructMemory();
					Options.SupplementDiagnosticSource = [SourceUnit, RowPath, SourceStruct, SourceData](
						UE::UAI::Contract::FDiagnostic& Error, const FJsonObject& Descriptor, const UStruct* RootStruct, const void* RootData, int32)
					{
						if (RootStruct == SourceStruct && RootData == SourceData)
							BindDataTableRowSource(Error, *SourceUnit, RowPath, Descriptor);
					};
				}
				UeAgentAuthoringObjectApply::FPreparedApply Prepared;
				UeAgentAuthoringObjectApply::FFailure PropertyFailure;
				UeAgentAuthoringObjectApply::FApplyResult Result;
				if (!UeAgentAuthoringObjectApply::PrepareStruct(Target.RowStruct.Get(), Data->GetStructMemory(),
					&Target, Descriptors, Options, Prepared, PropertyFailure))
					return Fail(Failure, PropertyFailure.Diagnostic);
				if (!UeAgentAuthoringObjectApply::ApplyPrepared(Prepared, Result))
					return Fail(Failure, Result.Diagnostic);
				Storage.Add(Name, Data);
				Views.Add(Name, Data->GetStructMemory());
				FRowIntent& Intent = Intents.AddDefaulted_GetRef();
				Intent.Name = Name;
				Intent.Storage = Data;
				Intent.Prepared = MoveTemp(Prepared);
			}
            Contract::FDiagnostic RowError;
            if (!AssetDataSimpleEffect::TryReplaceDataTableRows(Target, Target.RowStruct.Get(), Views, RowError))
            {
                RowError.TargetPath = LogicalTarget;
                return Fail(Failure, Contract::FDiagnostic::WrapFailure(TEXT("data_table_rows_replace_failed"), RowError, TEXT("authoring_apply")));
            }
			// Native row callbacks execute when replacing the table. Compare against
			// the original prepared intent on its real storage, not the scratch row.
			if (!VerifyRows(Target, Intents, Failure, LogicalTarget)) return false;
			if (OutIntents) *OutIntents = MoveTemp(Intents);
			return true;
		}

		bool CaptureRows(UDataTable& Table, TSharedPtr<FJsonObject>& Rows, FOperationFailure& Failure)
		{
			TArray<UeAgentAuthoringState::FReadRequest> Requests;
			UE::UAI::Contract::FDiagnostic Error;
			UeAgentAuthoringState::FCaptureResult Capture;
			if (!BuildDataTableDomainReadRequests(Table, Table.GetPathName(), Requests, Error)
				|| !UeAgentAuthoringState::CaptureSnapshot(Requests, Capture))
				return Fail(Failure, Error.IsEmpty() ? Capture.Diagnostic : Error);
			for (const auto& Entry : Capture.Snapshot.CopyEntries())
			{
				if (Entry.SemanticId != TEXT("data_table.rows")) continue;
				const TSharedPtr<FJsonObject>* CapturedRows = nullptr;
				if (Entry.Value.IsValid() && Entry.Value->AsObject()->TryGetObjectField(TEXT("rows"), CapturedRows))
				{
					Rows = *CapturedRows;
					return true;
				}
			}
			return Fail(Failure, DataLifecycleFailure(TEXT("data_table_rows_capture_missing"),
				TEXT("The native DataTable capture returned no rows object."), Table.GetPathName(), TEXT("/captured_rows_present"), false, true));
		}

		void ReleaseScratch(TStrongObjectPtr<UDataTable>& Table)
		{
			if (Table.IsValid())
			{
				Table->ClearFlags(RF_Public | RF_Standalone);
				Table->MarkAsGarbage();
				Table.Reset();
			}
		}

		class FTableState final : public IReflectionOperationLifecycleState
		{
		public:
			FTableState(FString InPath, UDataTable* InActual, UDataTable* InProposed, TSharedPtr<FJsonObject> InRows,
				TArray<FRowIntent>&& InRowIntents, TSharedPtr<const UeAgentAuthoringWorkspaceRouter::FWorkspaceUnit> InSourceUnit)
				: Path(MoveTemp(InPath)), Actual(InActual), Proposed(InProposed), DesiredRows(MoveTemp(InRows)),
				ProposedRowIntents(MoveTemp(InRowIntents)), SourceUnit(MoveTemp(InSourceUnit)), bCreate(InActual == nullptr)
			{
				if (bCreate)
				{
					CreationDefaults.Reset(NewObject<UDataTable>(GetTransientPackage()));
					CreationDefaults->RowStruct = InProposed->RowStruct;
				}
			}
			~FTableState() override { ProposedRowIntents.Reset(); ReleaseScratch(Proposed); ReleaseScratch(Before); ReleaseScratch(CreationDefaults); }
			FString GetTargetKey() const override { return Path; }
			EOperation GetResolvedOperation() const override { return bCreate ? EOperation::Create : EOperation::Update; }
			bool IsNoOp() const override { return !bCreate && !DesiredRows.IsValid(); }
			FReflectionRoot GetProposedRoot() const override { return FReflectionRoot::MakeObject(Proposed.Get()); }
			FReflectionRoot GetActualRoot() const override { return FReflectionRoot::MakeObject(Actual); }
			bool ShouldApplyPropertiesToActualRoot() const override { return true; }
			bool CaptureRollback(FOperationFailure& Failure) override
			{
				if (bCreate) return IsDataTableTargetAbsent(Path) || Fail(Failure, DataLifecycleFailure(TEXT("data_table_create_target_exists"), TEXT("The DataTable target is already present in memory, on disk or in the asset registry."), Path, TEXT("/target_absent"), false, true));
				if (!DesiredRows.IsValid()) return true;
				if (!Actual) return Fail(Failure, DataLifecycleFailure(TEXT("data_table_rollback_capture_failed"),
					TEXT("The actual DataTable is missing before rollback capture."), Path, TEXT("/actual_object_present"), false, true));
				Before.Reset(NewObject<UDataTable>(GetTransientPackage()));
				// Retain the original row values and object references; typed capture below freezes editable object state.
                if (!Before.IsValid())
                    return Fail(Failure, DataLifecycleFailure(TEXT("data_table_rollback_capture_failed"),
                        TEXT("The before DataTable snapshot object could not be created."), Path, TEXT("/before_snapshot_present"), false, true));
                Contract::FDiagnostic RowError;
                if (!ReplaceRows(*Before, *Actual, RowError))
                {
                    RowError.TargetPath = Path;
                    return Fail(Failure, Contract::FDiagnostic::WrapFailure(TEXT("data_table_rollback_capture_failed"), RowError, TEXT("authoring_apply")));
                }
				return CaptureRows(*Actual, BeforeRows, Failure);
			}
			bool ApplyProposed(FOperationFailure&) override { return true; }
			bool BuildDomainExpectedReadRequests(TArray<UeAgentAuthoringState::FReadRequest>& Requests,
				FOperationFailure& Failure) const override
			{
				if (!Proposed.IsValid()) return Fail(Failure, DataLifecycleFailure(TEXT("data_table_expected_state_missing"),
					TEXT("The proposed DataTable is missing while building expected read requests."), Path, TEXT("/proposed_object_present"), false, true));
				return VerifyRows(*Proposed, ProposedRowIntents, Failure, Path)
					&& Read(Proposed.Get(), Requests, Failure);
			}
			bool BuildDomainActualReadRequests(TArray<UeAgentAuthoringState::FReadRequest>& Requests,
				FOperationFailure& Failure) const override
			{
				return bCreate && !Actual ? ReadPresence(Requests, Failure) : Read(Actual, Requests, Failure);
			}
			bool BuildDomainBeforeReadRequests(TArray<UeAgentAuthoringState::FReadRequest>& Requests,
				FOperationFailure& Failure) const override
			{
				return bCreate ? ReadPresence(Requests, Failure) : BuildDomainActualReadRequests(Requests, Failure);
			}
			bool ApplyLifecycle(FOperationFailure& Failure) override
			{
				if (bCreate)
				{
					if (!IsDataTableTargetAbsent(Path)) return Fail(Failure, DataLifecycleFailure(TEXT("data_table_create_target_exists"), TEXT("The DataTable target is already present in memory, on disk or in the asset registry."), Path, TEXT("/target_absent"), false, true));
					const FString PackageName = FPackageName::ObjectPathToPackageName(Path);
					UPackage* Package = CreatePackage(*PackageName);
					// Native construction records transactional creation before NewObject returns.
					// Supply only the schema/defaults then; authored rows/properties apply below.
					Actual = NewObject<UDataTable>(Package, *FPackageName::GetLongPackageAssetName(PackageName),
						RF_Public | RF_Standalone | RF_Transactional, CreationDefaults.Get());
					if (!Actual) return Fail(Failure, DataLifecycleFailure(TEXT("data_table_create_failed"), TEXT("The native DataTable object could not be created."), Path, TEXT("/created_object_present"), false, true));
					bCreated = true;
					FAssetRegistryModule::AssetCreated(Actual);
					Actual->MarkPackageDirty();
				}
				if (!DesiredRows.IsValid()) return true;
				if (!Actual)
					return Fail(Failure, DataLifecycleFailure(TEXT("data_table_rows_replace_failed"),
						TEXT("The actual DataTable is missing before applying rows."), Path, TEXT("/actual_object_present"), false, true));
				if (!ApplyRows(*Actual, *DesiredRows, *Actual, Failure, nullptr, SourceUnit, Path)) return false;
				Actual->MarkPackageDirty();
				return true;
			}
			bool Compile(FOperationFailure&) override { return true; }
			bool Save(FOperationFailure&) override { return true; }
			bool BuildDurablePackagePlans(TArray<FPackageDurablePlan>& Plans,
				FOperationFailure& Failure) const override
			{
				if (!Actual && !bCreate) return Fail(Failure, DataLifecycleFailure(TEXT("data_table_target_missing"), TEXT("The DataTable lifecycle has no actual target object."), Path, TEXT("/actual_object_present"), false, true));
				return BuildPackageDurablePlan(Path, FPackageName::ObjectPathToPackageName(Path), Actual,
					EPackageDurableIntent::Save, Plans, Failure, true,
					bCreate ? EPackageBeforePresence::Absent : EPackageBeforePresence::Present);
			}
			bool Rollback(FOperationFailure& Failure) override
			{
				if (bCreate) return RemoveCreated(Failure);
				if (!DesiredRows.IsValid()) return true;
				if (!Actual || !Before.IsValid() || !BeforeRows.IsValid())
					return Fail(Failure, DataLifecycleFailure(TEXT("data_table_rows_restore_failed"), TEXT("The DataTable rollback is missing the actual object, before object or before rows."), Path, !Actual ? TEXT("/actual_object_present") : !Before.IsValid() ? TEXT("/before_object_present") : TEXT("/before_rows_present"), false, true));
				return ApplyRows(*Actual, *BeforeRows, *Before, Failure, nullptr, nullptr, Path);
			}
			bool SupportsPersistentReadback() const override { return true; }
			bool PrepareCheckpointRestore(FOperationFailure& Failure) override
			{
				return bCreate ? RemoveCreated(Failure) : ReleaseForPersistentReadback(Failure);
			}
			bool RebindAfterCheckpointRestore(FOperationFailure& Failure) override
			{
				return bCreate ? (IsDataTableTargetAbsent(Path) || Fail(Failure, DataLifecycleFailure(TEXT("data_table_absent_restore_unconfirmed"), TEXT("The newly created DataTable target is still present after rollback."), Path, TEXT("/target_absent"), false, true)))
					: RebindAfterPersistentReadback(Failure);
			}
			bool ReleaseForPersistentReadback(FOperationFailure&) override
			{
				ProposedRowIntents.Reset();
				Actual = nullptr;
				ReleaseScratch(Proposed);
				ReleaseScratch(Before);
				ReleaseScratch(CreationDefaults);
				return true;
			}
			bool RebindAfterPersistentReadback(FOperationFailure& Failure) override
			{
				Actual = LoadObject<UDataTable>(nullptr, *Path);
				return Actual || Fail(Failure, DataLifecycleFailure(TEXT("data_table_reload_failed"), TEXT("The persisted DataTable could not be reloaded for readback."), Path, TEXT("/readback_object_present"), false, true));
			}
		private:
			bool ReadPresence(TArray<UeAgentAuthoringState::FReadRequest>& Requests, FOperationFailure& Failure) const
			{
				UE::UAI::Contract::FDiagnostic Error;
				return BuildDataTablePresenceReadRequests(Path, Requests, Error) || Fail(Failure, Error);
			}
			bool RemoveCreated(FOperationFailure& Failure)
			{
				if (!bCreated) return IsDataTableTargetAbsent(Path) || Fail(Failure, DataLifecycleFailure(TEXT("data_table_absent_restore_unconfirmed"), TEXT("The newly created DataTable target is still present after rollback."), Path, TEXT("/target_absent"), false, true));
				UDataTable* Created = Actual ? Actual : FindObject<UDataTable>(nullptr, *Path);
				Actual = nullptr;
				if (Created)
				{
					const FWeakObjectPtr Witness(Created);
					const int32 Deleted = ObjectTools::ForceDeleteObjects({Created}, false);
					if (Deleted != 1)
					{
						auto Error = DataInputFailure(TEXT("data_table_created_object_delete_failed"), TEXT("Native deletion did not report deleting the newly created table."), Path, TEXT("/deleted_object_count"),
							MakeShared<FJsonValueNumber>(Deleted), MakeShared<FJsonValueNumber>(1));
						Error.SourceKind = TEXT("native_result"); Error.Source.JsonPath.Reset(); Error.SemanticPointer.Reset();
						return Fail(Failure, Error);
					}
					if (Witness.IsValid(true, true))
						return Fail(Failure, DataLifecycleFailure(TEXT("data_table_created_object_delete_failed"), TEXT("The newly created table remains alive after native deletion."), Path, TEXT("/created_object_alive"), true, false));
				}
				bCreated = false;
				return IsDataTableTargetAbsent(Path) || Fail(Failure, DataLifecycleFailure(TEXT("data_table_absent_restore_unconfirmed"), TEXT("The newly created DataTable target is still present after rollback."), Path, TEXT("/target_absent"), false, true));
			}
			bool Read(UDataTable* Table, TArray<UeAgentAuthoringState::FReadRequest>& Requests,
				FOperationFailure& Failure) const
			{
				if (!Table) return Fail(Failure, DataLifecycleFailure(TEXT("data_table_read_target_missing"),
					TEXT("The DataTable lifecycle read has no target object."), Path, TEXT("/read_target_present"), false, true));
				UE::UAI::Contract::FDiagnostic Error;
				return BuildDataTableDomainReadRequests(*Table, Path, Requests, Error)
					|| Fail(Failure, Error);
			}
			FString Path;
			UDataTable* Actual = nullptr;
			TStrongObjectPtr<UDataTable> Proposed;
			TStrongObjectPtr<UDataTable> Before;
			TStrongObjectPtr<UDataTable> CreationDefaults;
			TSharedPtr<FJsonObject> DesiredRows;
			TSharedPtr<FJsonObject> BeforeRows;
			mutable TArray<FRowIntent> ProposedRowIntents;
			TSharedPtr<const UeAgentAuthoringWorkspaceRouter::FWorkspaceUnit> SourceUnit;
			bool bCreate = false;
			bool bCreated = false;
		};

		class FTableLifecycle final : public IReflectionOperationLifecycle
		{
		public:
			bool PrepareLifecycle(const FPrepareRequest& Request,
				TUniquePtr<IReflectionOperationLifecycleState>& OutState, FOperationFailure& Failure) const override
			{
				if (!Request.IsValid())
					return Fail(Failure, DataLifecycleFailure(TEXT("data_table_operation_invalid"), TEXT("DataTable preparation requires a contract, workspace unit and apply input."), FString(), !Request.Contract ? TEXT("/contract_present") : !Request.Unit ? TEXT("/workspace_unit_present") : TEXT("/apply_input_present"), false, true));
				if (Request.Input->RequestedOperation != EOperation::Update && Request.Input->RequestedOperation != EOperation::Create)
					return Fail(Failure, DataInputFailure(TEXT("data_table_operation_invalid"), TEXT("This DataTable lifecycle supports create and update operations."), FString(), TEXT("/operation"), MakeShared<FJsonValueString>(OperationToString(Request.Input->RequestedOperation)), MakeShared<FJsonValueString>(TEXT("create or update"))));
				const bool bCreate = Request.Input->RequestedOperation == EOperation::Create;
				FString Path;
				FString StructPath;
				const TSharedPtr<FJsonObject>* Rows = nullptr;
				if (!Request.Input->Target.IsValid()
					|| !Request.Input->Target->TryGetStringField(TEXT("asset_path"), Path))
					return Fail(Failure, DataInputFailure(TEXT("data_table_apply_document_invalid"), TEXT("The DataTable target requires a string asset_path."), FString(), TEXT("/target/asset_path"), Request.Input->Target.IsValid() ? Request.Input->Target->Values.FindRef(TEXT("asset_path")) : nullptr, MakeShared<FJsonValueString>(TEXT("asset object path"))));
				const bool bHasRows = Request.Input->ProviderExtension.IsValid()
					&& !Request.Input->ProviderExtension->Values.IsEmpty();
				if (bHasRows && !Request.Input->ProviderExtension->TryGetStringField(TEXT("row_struct_ref"), StructPath))
					return Fail(Failure, DataInputFailure(TEXT("data_table_apply_document_invalid"), TEXT("The DataTable provider extension requires a string row_struct_ref."), Path, TEXT("/provider_extension/row_struct_ref"), Request.Input->ProviderExtension->Values.FindRef(TEXT("row_struct_ref")), MakeShared<FJsonValueString>(TEXT("UScriptStruct object path"))));
				if (bHasRows && !Request.Input->ProviderExtension->TryGetObjectField(TEXT("rows"), Rows))
					return Fail(Failure, DataInputFailure(TEXT("data_table_apply_document_invalid"), TEXT("The DataTable provider extension requires an object of rows."), Path, TEXT("/provider_extension/rows"), Request.Input->ProviderExtension->Values.FindRef(TEXT("rows")), MakeShared<FJsonValueString>(TEXT("object"))));
				if (bCreate)
				{
					const FString PackageName = FPackageName::ObjectPathToPackageName(Path);
					if (!FPackageName::IsValidLongPackageName(PackageName)
						|| !Path.Equals(HjsonWorkspace::DomainAuthoring::ObjectPathFromPackageIdentity(PackageName), ESearchCase::CaseSensitive))
						return Fail(Failure, DataInputFailure(TEXT("data_table_create_target_invalid"), TEXT("The create target must be a valid long package name with its matching asset object name."), Path, TEXT("/target/asset_path"), MakeShared<FJsonValueString>(Path), MakeShared<FJsonValueString>(TEXT("/Package/Asset.Asset"))));
					if (!IsDataTableTargetAbsent(Path)) return Fail(Failure, DataLifecycleFailure(TEXT("data_table_create_target_exists"), TEXT("The DataTable target is already present in memory, on disk or in the asset registry."), Path, TEXT("/target_absent"), false, true));
				}
				UDataTable* Actual = bCreate ? nullptr : LoadObject<UDataTable>(nullptr, *Path);
				if (!bCreate && !Actual)
					return Fail(Failure, DataLifecycleFailure(TEXT("data_table_row_struct_read_only"), TEXT("The update target could not be loaded as a DataTable."), Path, TEXT("/target_object_present"), false, true));
				if (!bCreate && !Actual->GetRowStruct())
					return Fail(Failure, DataLifecycleFailure(TEXT("data_table_row_struct_read_only"), TEXT("The existing DataTable has no row structure."), Path, TEXT("/row_struct_present"), false, true));
				if (!bCreate && bHasRows && Actual->GetRowStruct()->GetPathName() != StructPath)
					return Fail(Failure, DataInputFailure(TEXT("data_table_row_struct_read_only"), TEXT("The row structure of an existing DataTable cannot be changed by this import."), Path, TEXT("/provider_extension/row_struct_ref"), MakeShared<FJsonValueString>(StructPath), MakeShared<FJsonValueString>(Actual->GetRowStruct()->GetPathName())));
				TStrongObjectPtr<UDataTable> Proposed(bCreate ? NewObject<UDataTable>(GetTransientPackage()) : DuplicateObject<UDataTable>(Actual, GetTransientPackage()));
				if (!Proposed.IsValid()) return Fail(Failure, DataLifecycleFailure(TEXT("data_table_prepare_failed"), TEXT("The proposed DataTable could not be created or duplicated."), Path, TEXT("/proposed_object_present"), false, true));
				Proposed->ClearFlags(RF_Public | RF_Standalone);
				if (bCreate)
				{
					UScriptStruct* Struct = LoadObject<UScriptStruct>(nullptr, *StructPath);
					if (!bHasRows || !Struct || !FDataTableEditorUtils::IsValidTableStruct(Struct))
						return Fail(Failure, DataInputFailure(TEXT("data_table_row_struct_unsupported"), !bHasRows ? TEXT("DataTable creation requires a provider extension containing rows and row_struct_ref.") : !Struct ? TEXT("row_struct_ref could not be loaded as a UScriptStruct.") : TEXT("The requested UScriptStruct is not a valid DataTable row structure."), Path, !bHasRows ? TEXT("/provider_extension") : TEXT("/provider_extension/row_struct_ref"), !bHasRows ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(StructPath)), MakeShared<FJsonValueString>(TEXT("loadable valid DataTable row structure with rows input"))));
					Proposed->RowStruct = Struct;
				}
				TArray<FRowIntent> RowIntents;
				const TSharedPtr<const UeAgentAuthoringWorkspaceRouter::FWorkspaceUnit> SourceUnit =
					MakeShared<UeAgentAuthoringWorkspaceRouter::FWorkspaceUnit>(*Request.Unit);
				if (bHasRows && !ApplyRows(*Proposed, **Rows, *Proposed, Failure, &RowIntents, SourceUnit, Path)) return false;
				OutState = MakeUnique<FTableState>(Path, Actual, Proposed.Get(), bHasRows ? *Rows : nullptr, MoveTemp(RowIntents), SourceUnit);
				Proposed.Reset();
				return true;
			}
		};

		FContract MakeInputContract()
		{
			FContract Result;
			Result.Schema = TEXT("uai.apply_input_contract.v1");
			Result.ContractId = InputId;
			Result.ContractVersion = TEXT("1.0.0");
			Result.ProfileId = TEXT("data_table");
			Result.TargetKind = TEXT("data_table");
			Result.SemanticDescriptorRefs = {TEXT("data_table.rows"), TEXT("data_table.export.target")};
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
				{TEXT("rows"), TEXT("provider_extension"), TEXT("object")},
				{TEXT("row_schema"), TEXT("provider_extension"), TEXT("object")},
				{TEXT("row_struct_ref"), TEXT("provider_extension"), TEXT("struct_ref")}};
			FOperationDescriptor Operation;
			Operation.Operation = EOperation::Update;
			Operation.IdentityOneOf = {{TEXT("asset_path")}};
			Operation.AcceptedSections = {TEXT("target"), TEXT("properties"), TEXT("provider_extension")};
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
			Result.ProfileId = TEXT("data_table");
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
			Unit.RouteId = TEXT("data_table");
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

	bool ApplyDataTableAuthoring(const FString& Target, const FString& RequestId,
		const TSharedPtr<FJsonObject>& Desired,
		HjsonWorkspace::DomainAuthoring::FRootPropertyApplyOutcome& Outcome,
		const UeAgentAuthoringOperationApply::FBackendOptions* BackendOptions,
		const TArray<TSharedPtr<FJsonValue>>& Properties, bool bCreate)
	{
		using namespace UeAgentAuthoringOperationApply;
		const auto Contract = MakeWorkspaceContract();
		UE::UAI::Contract::FDiagnostic Error;
		UeAgentApplyInputContract::ERegistrationResult Registration;
		UeAgentAuthoringOperationApply::ERegistrationResult ProviderRegistration;
		FProviderRegistry Providers;
		UeAgentAuthoringObjectApply::FOptions PropertyOptions;
		PropertyOptions.bAllowDomainAuthorizedOwners = true;
		if (!UeAgentApplyInputContract::FRegistry::Get().RegisterContract(MakeInputContract(), Registration, Error)
			|| !Providers.RegisterProvider(MakeShared<FReflectionApplyProvider>(ProviderId,
				MakeShared<FTableLifecycle>(), PropertyOptions), ProviderRegistration, Error))
		{
			Outcome.Diagnostic = Error;
			return false;
		}
		const TSharedRef<FJsonObject> TargetObject = MakeShared<FJsonObject>();
		TargetObject->SetStringField(TEXT("asset_path"), Target);
		const TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
		Document->SetStringField(TEXT("operation"), bCreate ? TEXT("create") : TEXT("update"));
		Document->SetObjectField(TEXT("target"), TargetObject);
		if (Desired.IsValid()) Document->SetObjectField(TEXT("provider_extension"), Desired);
		if (!Properties.IsEmpty()) Document->SetArrayField(TEXT("properties"), Properties);
		UeAgentAuthoringWorkspaceLoader::FLoadResult Load;
		if (!UeAgentAuthoringWorkspaceLoader::LoadWorkspace(Contract,
			UeAgentAuthoringWorkspaceLoader::FSourceRequest::MakeInlineObject(Document, TEXT("inline://data-table")), Load))
		{
			Outcome.Diagnostic = Load.Diagnostic;
			return false;
		}
		FBackendOptions Options = BackendOptions ? *BackendOptions : FBackendOptions();
		Options.FinalReadback = Outcome.FinalReadback;
		Options.TransactionDescription = NSLOCTEXT("UeAgentInterface", "DataTableAuthoring", "Edit DataTable");
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

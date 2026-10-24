#include "DomainAdapters/DataTableCurveTable/UeAgentCurveTableAuthoringApply.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "DomainAdapters/DataTableCurveTable/UeAgentCurveTableAuthoringState.h"
#include "Engine/CurveTable.h"
#include "Curves/SimpleCurve.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "ObjectTools.h"
#include "Utilities/UeAgentCurveJson.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringObjectApplyProvider.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringAssetWorkspaceApply.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceLoader.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceRouter.h"

namespace UE::UAI::DomainAdapters::DataTableCurveTable
{
	namespace
	{
		using namespace UeAgentAuthoringOperationApply;
		using namespace UeAgentApplyInputContract;
		const FString ProviderId = TEXT("curve_table.authoring.apply");
		const FString InputId = TEXT("uai.apply_input.curve_table");
		const FString WorkspaceId = TEXT("uai.apply_workspace.curve_table");

		bool Fail(FOperationFailure& Failure, const UE::UAI::Contract::FDiagnostic& Diagnostic)
		{ Failure.Diagnostic = Diagnostic; return false; }

		UE::UAI::Contract::FDiagnostic CurveInputFailure(const FString& Code, const FString& Reason,
			const FString& Target, const FString& Field, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
		{
			auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Field, Expected, Actual, TEXT("authoring_apply"), Reason);
			Error.TargetPath = Target;
			return Error;
		}

		UE::UAI::Contract::FDiagnostic CurveLifecycleFailure(const FString& Code, const FString& Reason,
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

		void BindCurveTableInputSource(UE::UAI::Contract::FDiagnostic& Diagnostic,
			const UeAgentAuthoringWorkspaceRouter::FWorkspaceUnit& Unit)
		{
			if (Diagnostic.SourceKind != TEXT("native_result") && Diagnostic.SourceKind != TEXT("command_input")
				&& (Diagnostic.Source.SourceFile.IsEmpty() || Diagnostic.Source.SourceFile.StartsWith(TEXT("inline://"))
					|| Diagnostic.Source.SourceFile.StartsWith(TEXT("resolved://"))))
			{
				const FString Pointer = !Diagnostic.Source.InternalJsonPath.IsEmpty() ? Diagnostic.Source.InternalJsonPath
					: !Diagnostic.SemanticPointer.IsEmpty() ? Diagnostic.SemanticPointer : Diagnostic.Source.JsonPath;
				const auto Source = Unit.ResolveDiagnosticSource(TEXT("/provider_extension") + Pointer);
				if (!Source.SourceFile.IsEmpty() && !Source.SourceFile.StartsWith(TEXT("inline://"))
					&& !Source.SourceFile.StartsWith(TEXT("resolved://"))) Diagnostic.Source = Source;
				else
				{
					Diagnostic.Source.SourceFile.Reset(); Diagnostic.Source.Line = 0; Diagnostic.Source.Column = 0;
					Diagnostic.Source.JsonPath = Pointer; Diagnostic.Source.InternalJsonPath = Pointer;
				}
				Diagnostic.SemanticPointer = Pointer;
			}
			for (auto& Child : Diagnostic.Diagnostics) BindCurveTableInputSource(Child, Unit);
			if (Diagnostic.PublicCause)
			{
				auto Cause = MakeShared<UE::UAI::Contract::FDiagnostic>(*Diagnostic.PublicCause);
				BindCurveTableInputSource(*Cause, Unit); Diagnostic.PublicCause = MoveTemp(Cause);
			}
		}

		void RemoveRowsPreservingMode(UCurveTable& Table)
		{
			if (Table.GetCurveTableMode() == ECurveTableMode::Empty) return;
			TArray<FName> Names;
			if (Table.GetCurveTableMode() == ECurveTableMode::SimpleCurves) Table.GetSimpleCurveRowMap().GetKeys(Names);
			else Table.GetRichCurveRowMap().GetKeys(Names);
			for (FName Name : Names) Table.RemoveRow(Name);
		}

		void NotifyRowsChanged(UCurveTable& Table)
		{
			if (Table.HasAnyFlags(RF_LoadCompleted) || !Table.HasAnyFlags(RF_WasLoaded))
				UCurveTable::InvalidateAllCachedCurves();
			Table.OnCurveTableChanged().Broadcast();
		}

		void CopyRows(UCurveTable& Target, const UCurveTable& Source)
		{
			if (Source.GetCurveTableMode() == ECurveTableMode::Empty) { Target.EmptyTable(); return; }
			RemoveRowsPreservingMode(Target);
			if (Source.GetCurveTableMode() == ECurveTableMode::SimpleCurves)
				for (const auto& Pair : Source.GetSimpleCurveRowMap()) Target.AddSimpleCurve(Pair.Key) = *Pair.Value;
			else
				for (const auto& Pair : Source.GetRichCurveRowMap()) Target.AddRichCurve(Pair.Key) = *Pair.Value;
			NotifyRowsChanged(Target);
		}

		bool ApplyRows(UCurveTable& Target, const FJsonObject& Desired, FOperationFailure& Failure, const FString& LogicalTarget)
		{
			auto Invalid = [&](const TCHAR* Code, const TCHAR* Reason, const FString& Pointer,
				TSharedPtr<FJsonValue> Actual, const FString& Expected)
			{
				auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Pointer,
					MakeShared<FJsonValueString>(Expected), Actual, TEXT("authoring_apply"), Reason);
				Error.TargetPath = LogicalTarget;
				return Fail(Failure, Error);
			};
			FString Mode;
			const TSharedPtr<FJsonObject>* Rows = nullptr;
			if (!Desired.TryGetObjectField(TEXT("rows"), Rows) || !Rows || !Rows->IsValid())
				return Invalid(TEXT("curve_table_rows_required"), TEXT("CurveTable authoring requires a rows object."),
					TEXT("/rows"), Desired.TryGetField(TEXT("rows")), TEXT("object"));
			if (!Desired.HasTypedField<EJson::String>(TEXT("table_curve_mode")) || !Desired.TryGetStringField(TEXT("table_curve_mode"), Mode))
				return Invalid(TEXT("curve_table_mode_required"), TEXT("CurveTable authoring requires a string table_curve_mode."),
					TEXT("/table_curve_mode"), Desired.TryGetField(TEXT("table_curve_mode")), TEXT("string"));
			const ECurveTableMode BeforeMode = Target.GetCurveTableMode();
			if (BeforeMode == ECurveTableMode::Empty)
			{
				if (Mode == TEXT("empty") && (*Rows)->Values.IsEmpty()) return true;
				if ((*Rows)->Values.IsEmpty() || (Mode != TEXT("simple") && Mode != TEXT("rich")))
					return Invalid(TEXT("curve_table_first_row_mode_required"),
						(*Rows)->Values.IsEmpty() ? TEXT("A non-empty curve mode requires at least one initial row.")
							: TEXT("The first CurveTable rows require simple or rich mode."),
						(*Rows)->Values.IsEmpty() ? TEXT("/rows") : TEXT("/table_curve_mode"),
						(*Rows)->Values.IsEmpty() ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(0))
							: TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Mode)),
						(*Rows)->Values.IsEmpty() ? TEXT("one or more rows") : TEXT("simple or rich"));
			}
			else if (Mode != (BeforeMode == ECurveTableMode::SimpleCurves ? TEXT("simple") : TEXT("rich")))
				return Invalid(TEXT("curve_table_mode_read_only"), TEXT("An existing CurveTable cannot change its native curve mode."),
					TEXT("/table_curve_mode"), MakeShared<FJsonValueString>(Mode),
					BeforeMode == ECurveTableMode::SimpleCurves ? TEXT("simple") : TEXT("rich"));
			const bool bSimple = Mode == TEXT("simple");
			TMap<FName, FSimpleCurve> Simple;
			TMap<FName, FRichCurve> Rich;
			TSet<FName> Names;
			for (const auto& Pair : (*Rows)->Values)
			{
				const FString RowPath = TEXT("/rows/") + Pair.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
				const FName Name(*Pair.Key);
				if (Name.IsNone() || Names.Contains(Name) || !Pair.Value.IsValid() || Pair.Value->Type != EJson::Object)
				{
					if (Name.IsNone() || Names.Contains(Name))
						return Invalid(TEXT("curve_table_row_invalid"),
							Name.IsNone() ? TEXT("The CurveTable row key resolves to the reserved None name.")
								: TEXT("Two CurveTable row keys resolve to the same case-insensitive native name."),
							RowPath, MakeShared<FJsonValueString>(Pair.Key), TEXT("a unique non-None row name"));
					return Invalid(TEXT("curve_table_row_invalid"), TEXT("The CurveTable row must be an object."),
						RowPath, Pair.Value, TEXT("object"));
				}
				Names.Add(Name);
				const TSharedPtr<FJsonObject>* Properties = nullptr;
				const TSharedPtr<FJsonObject>* Property = nullptr;
				const TSharedPtr<FJsonObject>* Value = nullptr;
				FString Type;
				if (!Pair.Value->AsObject()->TryGetObjectField(TEXT("properties"), Properties))
					return Invalid(TEXT("curve_table_curve_value_invalid"), TEXT("The CurveTable row requires a properties object."),
						RowPath + TEXT("/properties"), Pair.Value->AsObject()->TryGetField(TEXT("properties")), TEXT("object containing only Curve"));
				if ((*Properties)->Values.Num() != 1)
					return Invalid(TEXT("curve_table_curve_value_invalid"), TEXT("The CurveTable row must contain exactly one Curve property."),
						RowPath + TEXT("/properties"), MakeShared<FJsonValueNumber>((*Properties)->Values.Num()), TEXT("exactly one Curve property"));
				if (!(*Properties)->TryGetObjectField(TEXT("Curve"), Property))
					return Invalid(TEXT("curve_table_curve_value_invalid"), TEXT("The row properties must contain a Curve object."),
						RowPath + TEXT("/properties/Curve"), (*Properties)->TryGetField(TEXT("Curve")), TEXT("object"));
				if (!(*Property)->TryGetStringField(TEXT("type"), Type) || Type != TEXT("curve"))
					return Invalid(TEXT("curve_table_curve_value_invalid"), TEXT("The Curve property descriptor type must be curve."),
						RowPath + TEXT("/properties/Curve/type"), (*Property)->TryGetField(TEXT("type")), TEXT("curve"));
				if (!(*Property)->TryGetObjectField(TEXT("value"), Value))
					return Invalid(TEXT("curve_table_curve_value_invalid"), TEXT("The Curve property requires an object value."),
						RowPath + TEXT("/properties/Curve/value"), (*Property)->TryGetField(TEXT("value")), TEXT("object"));
				const FString CurvePointer = TEXT("/rows/")
					+ Pair.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"))
					+ TEXT("/properties/Curve/value");
				TArray<UE::UAI::Contract::FDiagnostic> Issues;
				bool bApplied;
				if (bSimple)
				{
					FSimpleCurve Curve;
					if (BeforeMode == ECurveTableMode::SimpleCurves)
						if (const auto* Existing = Target.GetSimpleCurveRowMap().Find(Name)) Curve = **Existing;
					bApplied = UeAgentCurveJson::ApplyChannelJsonToSimpleCurve(*Value, TEXT("value"), Curve, CurvePointer, Issues);
					if (bApplied) Simple.Add(Name, MoveTemp(Curve));
				}
				else
				{
					FRichCurve Curve;
					if (BeforeMode == ECurveTableMode::RichCurves)
						if (const auto* Existing = Target.GetRichCurveRowMap().Find(Name)) Curve = **Existing;
					bApplied = UeAgentCurveJson::ApplyChannelJsonToRichCurve(*Value, TEXT("value"), Curve, CurvePointer, Issues);
					if (bApplied) Rich.Add(Name, MoveTemp(Curve));
				}
				if (!bApplied)
				{
					const auto Diagnostic = UeAgentAuthoringDiagnostics::MakeIssuesFailure(
						TEXT("curve_table_curve_value_invalid"), Issues, TEXT("curve_decode"), LogicalTarget);
					return Fail(Failure, Diagnostic);
				}
			}
			RemoveRowsPreservingMode(Target);
			for (const auto& Pair : Simple) Target.AddSimpleCurve(Pair.Key) = Pair.Value;
			for (const auto& Pair : Rich) Target.AddRichCurve(Pair.Key) = Pair.Value;
			NotifyRowsChanged(Target);
			return true;
		}

		void Release(TStrongObjectPtr<UCurveTable>& Table)
		{ if (Table.IsValid()) { Table->ClearFlags(RF_Public | RF_Standalone); Table->MarkAsGarbage(); Table.Reset(); } }

		class FTableState final : public IReflectionOperationLifecycleState
		{
		public:
			FTableState(FString InPath, UCurveTable* InActual, UCurveTable* InProposed, bool bInRows)
				: Path(MoveTemp(InPath)), Actual(InActual), Proposed(InProposed), bRows(bInRows), bCreate(!InActual) {}
			~FTableState() override { Release(Proposed); Release(Before); }
			FString GetTargetKey() const override { return Path; }
			EOperation GetResolvedOperation() const override { return bCreate ? EOperation::Create : EOperation::Update; }
			bool IsNoOp() const override { return !bCreate && !bRows; }
			FReflectionRoot GetProposedRoot() const override { return FReflectionRoot::MakeObject(Proposed.Get()); }
			FReflectionRoot GetActualRoot() const override { return FReflectionRoot::MakeObject(Actual); }
			bool ShouldApplyPropertiesToActualRoot() const override { return true; }
			bool CaptureRollback(FOperationFailure& Failure) override
			{
				if (bCreate || !bRows) return true;
				Before.Reset(DuplicateObject<UCurveTable>(Actual, GetTransientPackage()));
				return Before.IsValid() || Fail(Failure, CurveLifecycleFailure(TEXT("curve_table_before_capture_failed"), TEXT("The native CurveTable could not be duplicated for rollback capture."), Path, TEXT("/before_snapshot_present"), false, true));
			}
			bool ApplyProposed(FOperationFailure&) override { return true; }
			bool BuildDomainExpectedReadRequests(TArray<UeAgentAuthoringState::FReadRequest>& R, FOperationFailure& E) const override { return Read(Proposed.Get(), R, E); }
			bool BuildDomainActualReadRequests(TArray<UeAgentAuthoringState::FReadRequest>& R, FOperationFailure& E) const override { return bCreate && !Actual ? ReadPresence(R, E) : Read(Actual, R, E); }
			bool BuildDomainBeforeReadRequests(TArray<UeAgentAuthoringState::FReadRequest>& R, FOperationFailure& E) const override { return bCreate ? ReadPresence(R, E) : Read(Actual, R, E); }
			bool ApplyLifecycle(FOperationFailure& Failure) override
			{
				if (bCreate)
				{
					if (!IsCurveTableTargetAbsent(Path)) return Fail(Failure, CurveLifecycleFailure(TEXT("curve_table_create_target_exists"), TEXT("The CurveTable target is already present in memory, on disk or in the asset registry."), Path, TEXT("/target_absent"), false, true));
					const FString PackageName = FPackageName::ObjectPathToPackageName(Path);
					UPackage* Package = CreatePackage(*PackageName);
					Actual = NewObject<UCurveTable>(Package, *FPackageName::GetLongPackageAssetName(PackageName),
						RF_Public | RF_Standalone | RF_Transactional);
					if (!Actual) return Fail(Failure, CurveLifecycleFailure(TEXT("curve_table_create_failed"), TEXT("The native CurveTable object could not be created."), Path, TEXT("/created_object_present"), false, true));
					bCreated = true;
					FAssetRegistryModule::AssetCreated(Actual);
					Actual->MarkPackageDirty();
				}
				if (!bRows) return true;
				if (!Actual || !Proposed.IsValid()) return Fail(Failure, CurveLifecycleFailure(TEXT("curve_table_apply_state_missing"), TEXT("The CurveTable lifecycle cannot apply without both actual and proposed objects."), Path, Actual ? TEXT("/proposed_object_present") : TEXT("/actual_object_present"), false, true));
				Actual->Modify(); CopyRows(*Actual, *Proposed); Actual->MarkPackageDirty(); return true;
			}
			bool Compile(FOperationFailure&) override { return true; }
			bool Save(FOperationFailure&) override { return true; }
			bool Rollback(FOperationFailure& Failure) override
			{
				if (bCreate) return RemoveCreated(Failure);
				if (!bRows) return true;
				if (!Actual || !Before.IsValid()) return Fail(Failure, CurveLifecycleFailure(TEXT("curve_table_restore_state_missing"), TEXT("The CurveTable rollback cannot restore without both actual object and before snapshot."), Path, Actual ? TEXT("/before_snapshot_present") : TEXT("/actual_object_present"), false, true));
				CopyRows(*Actual, *Before); return true;
			}
			bool BuildDurablePackagePlans(TArray<FPackageDurablePlan>& Plans, FOperationFailure& Failure) const override
			{ return BuildPackageDurablePlan(Path, FPackageName::ObjectPathToPackageName(Path), Actual, EPackageDurableIntent::Save, Plans, Failure,
				true, bCreate ? EPackageBeforePresence::Absent : EPackageBeforePresence::Present); }
			bool SupportsPersistentReadback() const override { return true; }
			bool PrepareCheckpointRestore(FOperationFailure& Failure) override
			{ return bCreate ? RemoveCreated(Failure) : ReleaseForPersistentReadback(Failure); }
			bool RebindAfterCheckpointRestore(FOperationFailure& Failure) override
			{ return bCreate ? (IsCurveTableTargetAbsent(Path) || Fail(Failure, CurveLifecycleFailure(TEXT("curve_table_absent_restore_unconfirmed"), TEXT("The newly created CurveTable target is still present after rollback."), Path, TEXT("/target_absent"), false, true))) : RebindAfterPersistentReadback(Failure); }
			bool ReleaseForPersistentReadback(FOperationFailure&) override { Actual = nullptr; Release(Proposed); Release(Before); return true; }
			bool RebindAfterPersistentReadback(FOperationFailure& Failure) override
			{ Actual = LoadObject<UCurveTable>(nullptr, *Path); return Actual || Fail(Failure, CurveLifecycleFailure(TEXT("curve_table_reload_failed"), TEXT("The persisted CurveTable could not be reloaded for readback."), Path, TEXT("/readback_object_present"), false, true)); }
		private:
			bool ReadPresence(TArray<UeAgentAuthoringState::FReadRequest>& Requests, FOperationFailure& Failure) const
			{
				UE::UAI::Contract::FDiagnostic Error;
				return BuildCurveTablePresenceReadRequests(Path, Requests, Error) || Fail(Failure, Error);
			}
			bool RemoveCreated(FOperationFailure& Failure)
			{
				if (!bCreated) return IsCurveTableTargetAbsent(Path) || Fail(Failure, CurveLifecycleFailure(TEXT("curve_table_absent_restore_unconfirmed"), TEXT("The newly created CurveTable target is still present after rollback."), Path, TEXT("/target_absent"), false, true));
				UCurveTable* Created = Actual ? Actual : FindObject<UCurveTable>(nullptr, *Path);
				Actual = nullptr;
				if (Created)
				{
					const FWeakObjectPtr Witness(Created);
					const int32 Deleted = ObjectTools::ForceDeleteObjects({Created}, false);
					if (Deleted != 1)
					{
						auto Error = CurveInputFailure(TEXT("curve_table_created_object_delete_failed"), TEXT("Native deletion did not report deleting the newly created table."), Path, TEXT("/deleted_object_count"),
							MakeShared<FJsonValueNumber>(Deleted), MakeShared<FJsonValueNumber>(1));
						Error.SourceKind = TEXT("native_result"); Error.Source.JsonPath.Reset(); Error.SemanticPointer.Reset();
						return Fail(Failure, Error);
					}
					if (Witness.IsValid(true, true))
						return Fail(Failure, CurveLifecycleFailure(TEXT("curve_table_created_object_delete_failed"), TEXT("The newly created table remains alive after native deletion."), Path, TEXT("/created_object_alive"), true, false));
				}
				bCreated = false;
				return IsCurveTableTargetAbsent(Path) || Fail(Failure, CurveLifecycleFailure(TEXT("curve_table_absent_restore_unconfirmed"), TEXT("The newly created CurveTable target is still present after rollback."), Path, TEXT("/target_absent"), false, true));
			}
			bool Read(UCurveTable* Table, TArray<UeAgentAuthoringState::FReadRequest>& Requests, FOperationFailure& Failure) const
			{
				if (!Table) return Fail(Failure, CurveLifecycleFailure(TEXT("curve_table_read_target_missing"),
					TEXT("The CurveTable lifecycle read has no target object."), Path, TEXT("/read_target_present"), false, true));
				UE::UAI::Contract::FDiagnostic Error;
				return BuildCurveTableDomainReadRequests(*Table, Path, Requests, Error) || Fail(Failure, Error);
			}
			FString Path;
			UCurveTable* Actual;
			TStrongObjectPtr<UCurveTable> Proposed, Before;
			bool bRows;
			bool bCreate = false;
			bool bCreated = false;
		};

		class FTableLifecycle final : public IReflectionOperationLifecycle
		{
			bool PrepareLifecycle(const FPrepareRequest& Request, TUniquePtr<IReflectionOperationLifecycleState>& OutState, FOperationFailure& Failure) const override
			{
				if (!Request.IsValid())
					return Fail(Failure, CurveLifecycleFailure(TEXT("curve_table_operation_invalid"), TEXT("CurveTable preparation requires a contract, workspace unit and apply input."), FString(), !Request.Contract ? TEXT("/contract_present") : !Request.Unit ? TEXT("/workspace_unit_present") : TEXT("/apply_input_present"), false, true));
				if (Request.Input->RequestedOperation != EOperation::Update && Request.Input->RequestedOperation != EOperation::Create)
					return Fail(Failure, CurveInputFailure(TEXT("curve_table_operation_invalid"), TEXT("This CurveTable lifecycle supports create and update operations."), FString(), TEXT("/operation"), MakeShared<FJsonValueString>(OperationToString(Request.Input->RequestedOperation)), MakeShared<FJsonValueString>(TEXT("create or update"))));
				const bool bCreate = Request.Input->RequestedOperation == EOperation::Create;
				FString Path;
				if (!Request.Input->Target.IsValid() || !Request.Input->Target->TryGetStringField(TEXT("asset_path"), Path))
					return Fail(Failure, CurveInputFailure(TEXT("curve_table_target_invalid"), TEXT("The CurveTable target requires a string asset_path."), FString(), TEXT("/target/asset_path"), Request.Input->Target.IsValid() ? Request.Input->Target->Values.FindRef(TEXT("asset_path")) : nullptr, MakeShared<FJsonValueString>(TEXT("asset object path"))));
				if (bCreate)
				{
					const FString PackageName = FPackageName::ObjectPathToPackageName(Path);
					if (!FPackageName::IsValidLongPackageName(PackageName)
						|| !Path.Equals(HjsonWorkspace::DomainAuthoring::ObjectPathFromPackageIdentity(PackageName), ESearchCase::CaseSensitive))
						return Fail(Failure, CurveInputFailure(TEXT("curve_table_create_target_invalid"), TEXT("The create target must be a valid long package name with its matching asset object name."), Path, TEXT("/target/asset_path"), MakeShared<FJsonValueString>(Path), MakeShared<FJsonValueString>(TEXT("/Package/Asset.Asset"))));
					if (!IsCurveTableTargetAbsent(Path)) return Fail(Failure, CurveLifecycleFailure(TEXT("curve_table_create_target_exists"), TEXT("The CurveTable target is already present in memory, on disk or in the asset registry."), Path, TEXT("/target_absent"), false, true));
				}
				UCurveTable* Actual = bCreate ? nullptr : LoadObject<UCurveTable>(nullptr, *Path);
				if (!bCreate && !Actual) return Fail(Failure, CurveLifecycleFailure(TEXT("curve_table_target_missing"), TEXT("The update target could not be loaded as a CurveTable."), Path, TEXT("/target_object_present"), false, true));
				TStrongObjectPtr<UCurveTable> Proposed(bCreate ? NewObject<UCurveTable>(GetTransientPackage()) : DuplicateObject<UCurveTable>(Actual, GetTransientPackage()));
				if (!Proposed.IsValid()) return Fail(Failure, CurveLifecycleFailure(TEXT("curve_table_prepare_failed"), TEXT("The proposed CurveTable could not be created or duplicated."), Path, TEXT("/proposed_object_present"), false, true));
				Proposed->ClearFlags(RF_Public | RF_Standalone);
				const bool bRows = Request.Input->ProviderExtension.IsValid() && !Request.Input->ProviderExtension->Values.IsEmpty();
				if (bRows && !ApplyRows(*Proposed, *Request.Input->ProviderExtension, Failure, Path))
				{
					BindCurveTableInputSource(Failure.Diagnostic, *Request.Unit);
					return false;
				}
				OutState = MakeUnique<FTableState>(Path, Actual, Proposed.Get(), bRows);
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
			Result.ProfileId = TEXT("curve_table");
			Result.TargetKind = TEXT("curve_table");
			Result.SemanticDescriptorRefs = {TEXT("curve_table.rows"), TEXT("curve_table.export.target")};
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
				{TEXT("table_curve_mode"), TEXT("provider_extension"), TEXT("string")}};
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
			Result.ProfileId = TEXT("curve_table");
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
			Unit.RouteId = TEXT("curve_table");
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

	bool ApplyCurveTableAuthoring(const FString& Target, const FString& RequestId,
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
			UeAgentAuthoringWorkspaceLoader::FSourceRequest::MakeInlineObject(Document, TEXT("inline://curve-table")), Load))
		{
			Outcome.Diagnostic = Load.Diagnostic;
			return false;
		}
		FBackendOptions Options = BackendOptions ? *BackendOptions : FBackendOptions();
		Options.FinalReadback = Outcome.FinalReadback;
		Options.TransactionDescription = NSLOCTEXT("UeAgentInterface", "CurveTableAuthoring", "Edit CurveTable");
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

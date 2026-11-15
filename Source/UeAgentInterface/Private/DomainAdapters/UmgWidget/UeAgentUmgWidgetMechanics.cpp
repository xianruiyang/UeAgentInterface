#include "DomainAdapters/UmgWidget/UeAgentUmgWidgetMechanics.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Composition/HjsonImport/UeAgentSemanticWorkspaceImport.h"
#include "Blueprint/WidgetTree.h"
#include "Components/PanelSlot.h"
#include "Components/Widget.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceIO.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringRegistration.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringDescriptors.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringState.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintSemanticProjection.h"
#include "DomainAdapters/Blueprint/Surfaces/UeAgentBlueprintCandidateBindingNormalizer.h"
#include "Domains/UMG/Authoring/UeAgentUmgAuthoringApplyProvider.h"
#include "Domains/UMG/Authoring/UeAgentUmgAuthoringDescriptors.h"
#include "Domains/UMG/Authoring/UeAgentUmgAuthoringExport.h"
#include "Domains/UMG/Authoring/UeAgentUmgAuthoringRegistration.h"
#include "Domains/UMG/Authoring/UeAgentUmgAuthoringState.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "WidgetBlueprint.h"

namespace UE::UAI::DomainAdapters::UmgWidget
{
	using namespace CommandContract;
	using namespace HjsonWorkspace;
	using namespace HjsonWorkspace::DomainAuthoring;

	namespace
	{
		const FString SchemaId = TEXT("uai.umg.edit/1");
		const FString WidgetBlueprintType = TEXT("/Script/UMGEditor.WidgetBlueprint");

		Contract::FDiagnostic WidgetFact(const TCHAR* Code, const FString& Reason,
			const FString& Path, TSharedPtr<FJsonValue> Actual = nullptr,
			TSharedPtr<FJsonValue> Expected = nullptr, const FString& Target = FString(),
			const TCHAR* SourceKind = TEXT(""))
		{
			auto Error = Contract::FDiagnostic::Failure(Code, Reason, TEXT("authoring_apply"));
			Error.PropertyPath = Path;
			Error.SemanticPointer = Path;
			Error.Source.InternalJsonPath = Path;
			Error.SourceKind = SourceKind;
			Error.TargetPath = Target;
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			return Error;
		}

		FString FieldPath(const FString& Parent, FString Field)
		{
			Field.ReplaceInline(TEXT("~"), TEXT("~0"));
			Field.ReplaceInline(TEXT("/"), TEXT("~1"));
			return Parent + TEXT("/") + Field;
		}

		TSharedPtr<FJsonValue> JsonFact(const TSharedPtr<FJsonValue>& Value)
		{
			if (!Value.IsValid()) return nullptr;
			switch (Value->Type)
			{
			case EJson::Object: return MakeShared<FJsonValueString>(TEXT("object"));
			case EJson::Array: return MakeShared<FJsonValueString>(TEXT("array"));
			case EJson::None: return MakeShared<FJsonValueString>(TEXT("none"));
			default: return Value;
			}
		}

		Contract::FDiagnostic WidgetInput(const TCHAR* Code, const FString& Reason,
			const FString& Path, const TSharedPtr<FJsonValue>& Value, const FString& Expected,
			const FString& Target = FString(), const TCHAR* SourceKind = TEXT(""))
		{
			auto Error = WidgetFact(Code, Reason, Path, JsonFact(Value),
				MakeShared<FJsonValueString>(Expected), Target, SourceKind);
			Error.ActualValuePresent = Value.IsValid();
			Error.ExpectedValuePresent = true;
			if (Value.IsValid() && (Value->Type == EJson::Object || Value->Type == EJson::Array))
				Error.ActualEvidence = Value;
			return Error;
		}

		void LocateSemanticFailure(const FResolvedSemanticWorkspace& Workspace,
			const FString& UnitId, Contract::FDiagnostic& Error, const FString& FallbackPointer)
		{
			const bool bConcreteSource = !Error.Source.SourceFile.IsEmpty() && Error.Source.Line > 0
				&& !Error.Source.SourceFile.StartsWith(TEXT("inline://"))
				&& !Error.Source.SourceFile.StartsWith(TEXT("resolved:"));
			if (!bConcreteSource && Error.SourceKind != TEXT("native_result")
				&& Error.SourceKind != TEXT("command_input"))
			{
				if (Error.Source.SourceUnit.IsEmpty()) Error.Source.SourceUnit = UnitId;
				if (Error.Source.InternalJsonPath.IsEmpty()) Error.Source.InternalJsonPath =
					!Error.SemanticPointer.IsEmpty() ? Error.SemanticPointer : FallbackPointer;
				Workspace.LocateDocumentDiagnostic({}, {}, Error, UnitId);
			}
			if (Error.PublicCause)
			{
				auto Cause = *Error.PublicCause;
				LocateSemanticFailure(Workspace, UnitId, Cause, FallbackPointer);
				Error.PublicCause = MakeShared<Contract::FDiagnostic>(MoveTemp(Cause));
			}
			for (auto& Child : Error.Diagnostics) LocateSemanticFailure(Workspace, UnitId, Child, FallbackPointer);
		}

		bool Fail(FWorkspaceError& OutError, Contract::FDiagnostic Diagnostic)
		{
			OutError.SetFailure(MoveTemp(Diagnostic));
			return false;
		}
		FString PackagePath(FString Target)
		{
			Target.TrimStartAndEndInline();
			return Target.Contains(TEXT("."))
				? FPackageName::ObjectPathToPackageName(Target)
				: Target;
		}

		FString ObjectPath(const FString& Target)
		{
			if (Target.Contains(TEXT("."))) return Target;
			const FString Name = FPackageName::GetLongPackageAssetName(Target);
			return Name.IsEmpty() ? Target : Target + TEXT(".") + Name;
		}

		bool CanonicalObject(
			const TSharedPtr<FJsonObject>& Object,
			FString& Out,
			FWorkspaceError& OutError)
		{
			return TryCanonicalizeObject(Object, Out, OutError);
		}

		bool EnsureAuthoringRegistered(FWorkspaceError& OutError)
		{
			static const TPair<bool, UE::UAI::Contract::FDiagnostic> Registration = []()
			{
				UE::UAI::Contract::FDiagnostic Error;
				const bool bRegistered =
					UeAgentBlueprintAuthoring::RegisterAuthoring(Error)
					&& UeAgentUmgAuthoring::RegisterAuthoring(Error);
				return TPair<bool, UE::UAI::Contract::FDiagnostic>(bRegistered, MoveTemp(Error));
			}();
			if (!Registration.Key)
			{
				return Fail(OutError, Registration.Value);
			}
			return true;
		}

		UWidgetBlueprint* LoadWidgetBlueprint(const FString& Target)
		{
			return Cast<UWidgetBlueprint>(StaticLoadObject(
				UWidgetBlueprint::StaticClass(), nullptr, *ObjectPath(Target)));
		}

		bool TryRead(
			const FString& Target,
			FString& OutPayload,
			FString& OutRevision,
			FWorkspaceError& OutError)
		{
			UObject* Object = nullptr;
			DomainAdapters::ContentGraphRead::FUeAgentContentGraphCapture Capture;
			if (!TryCaptureObject(
					Target, WidgetBlueprintType, Object, Capture, OutError))
			{
				return false;
			}
			TSharedPtr<FJsonObject> Snapshot;
			if (!TryBuildCaptureSnapshot(
					Target, WidgetBlueprintType, Capture, Snapshot, OutError))
			{
				return false;
			}
			OutRevision = Capture.Revision;
			return TryCanonicalizeObject(Snapshot, OutPayload, OutError);
		}

		bool TryOperation(
			FString Operation,
			UeAgentApplyInputContract::EOperation& OutOperation)
		{
			Operation.TrimStartAndEndInline();
			Operation.ToLowerInline();
			if (Operation == TEXT("create"))
			{
				OutOperation = UeAgentApplyInputContract::EOperation::Create;
				return true;
			}
			if (Operation == TEXT("update"))
			{
				OutOperation = UeAgentApplyInputContract::EOperation::Update;
				return true;
			}
			if (Operation == TEXT("upsert"))
			{
				OutOperation = UeAgentApplyInputContract::EOperation::Upsert;
				return true;
			}
			return false;
		}

		bool TryReadbackUmgImportResult(
			const FString& Target,
			FString& OutPayload,
			FWorkspaceError& OutError)
		{
			return TryReadbackImportPayload(
				TEXT("umg-widget-domain-provider"),
				Target,
				ObjectPath(Target),
				nullptr,
				TEXT("umg_widget_readback_payload_invalid"),
				TEXT("umg_widget_compile_not_settled"),
				[&Target](FString& OutReadbackPayload, FString& OutRevision,
					FWorkspaceError& OutReadError)
				{
					return TryRead(Target, OutReadbackPayload, OutRevision, OutReadError);
				},
				OutPayload,
				OutError);
		}

		bool TryProjectWidgetChanges(
			const FAuthoringModelRef& DesiredModel,
			const FAuthoringModelRef& CurrentModel,
			const TArray<UeAgentAuthoringState::FReadRequest>& Requests,
			const UWidgetBlueprint& Blueprint,
			const Composition::HjsonImport::FSemanticWorkspaceChangeProjection& Projection,
			TSharedPtr<FJsonObject>& OutExtension,
			FWorkspaceError& OutError,
			const FResolvedSemanticWorkspace& Workspace)
		{
			const auto Reject = [&](Contract::FDiagnostic Error, const FString& UnitId,
				const FString& Pointer, const FString& Candidate = FString())
			{
				if (Error.TargetPath.IsEmpty()) Error.TargetPath = Blueprint.GetPathName();
				if (Error.CandidateName.IsEmpty()) Error.CandidateName = Candidate;
				if (Error.Source.SourceUnit.IsEmpty()) Error.Source.SourceUnit = UnitId;
				LocateSemanticFailure(Workspace, UnitId, Error, Pointer);
				return Fail(OutError, MoveTemp(Error));
			};
			TArray<FString> BlueprintUnitIds;
			TArray<FString> WidgetUnitIds;
			TArray<FStructuralEdit> BlueprintStructuralEdits;
			auto IsBlueprintMemberCollection = [&](const FString& UnitId)
			{
				const FSemanticUnit* Unit = DesiredModel->FindUnit(UnitId);
				if (!Unit) Unit = CurrentModel->FindUnit(UnitId);
				return Unit && Unit->GetUnitKind() == ESemanticUnitKind::StructureState
					&& Unit->GetRole() == TEXT("member_collection");
			};
			auto IsBlueprintGraphUnit = [&](const FString& UnitId)
			{
				const FSemanticUnit* Unit = DesiredModel->FindUnit(UnitId);
				if (!Unit) Unit = CurrentModel->FindUnit(UnitId);
				TSet<FString> Visited;
				while (Unit && !Visited.Contains(Unit->GetUnitId()))
				{
					if (Unit->GetSemanticTypeRef() == UeAgentBlueprintAuthoring::GraphSemanticId
						|| Unit->GetSemanticTypeRef() == TEXT("uai.authoring.surface.graph_collection")) return true;
					Visited.Add(Unit->GetUnitId());
					const FString Owner = Unit->GetOwnerRef();
					Unit = DesiredModel->FindUnit(Owner);
					if (!Unit) Unit = CurrentModel->FindUnit(Owner);
				}
				return false;
			};
			for (const FString& UnitId : Projection.ChangedUnitIds)
			{
				const FSemanticUnit* Unit = DesiredModel->FindUnit(UnitId);
				if (!Unit) return Reject(WidgetFact(TEXT("umg_semantic_unit_missing"),
					TEXT("The changed unit is missing from the desired semantic model."), TEXT("/"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)), UnitId, TEXT("/"));
				if (Unit->GetSemanticTypeRef() == TEXT("core.property")) continue;
				if (IsBlueprintMemberCollection(UnitId)
					|| Unit->GetSemanticTypeRef() == UeAgentBlueprintAuthoring::VariableSemanticId
					|| Unit->GetSemanticTypeRef() == UeAgentBlueprintAuthoring::DelegateSemanticId
					|| Unit->GetSemanticTypeRef() == UeAgentBlueprintAuthoring::InterfaceSemanticId)
				{
					BlueprintUnitIds.Add(UnitId);
					continue;
				}
				if (Unit->GetSemanticTypeRef() == UeAgentBlueprintAuthoring::ClassDefaultPropertySemanticId
					|| IsBlueprintGraphUnit(UnitId))
				{
					if (Unit->GetUnitKind() == ESemanticUnitKind::LayoutState)
					{
						// Ordinary graph pos is carried by its changed node; the hidden layout
						// is a derived projection and has no independent Blueprint mutation.
						if (!Projection.ChangedUnitIds.Contains(Unit->GetOwnerRef()))
						{
							auto Error = WidgetFact(TEXT("umg_blueprint_layout_owner_not_changed"),
								TEXT("A derived Blueprint graph layout change requires its owning node to be changed."),
								TEXT("/owner_ref"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
							Error.ExpectedEvidence = MakeShared<FJsonValueString>(Unit->GetOwnerRef());
							return Reject(MoveTemp(Error), UnitId, TEXT("/owner_ref"));
						}
						continue;
					}
					const FString& Semantic = Unit->GetSemanticTypeRef();
					if (Semantic != UeAgentBlueprintAuthoring::ClassDefaultPropertySemanticId
						&& Semantic != TEXT("uai.authoring.surface.graph_collection")
						&& Semantic != UeAgentBlueprintAuthoring::GraphSemanticId
						&& Semantic != UeAgentBlueprintAuthoring::NodeSemanticId
						&& Semantic != UeAgentBlueprintAuthoring::PinDefaultSemanticId
						&& Semantic != UeAgentBlueprintAuthoring::NodePropertySemanticId
						&& Semantic != UeAgentBlueprintAuthoring::EdgeSemanticId)
						return Reject(WidgetFact(TEXT("umg_blueprint_projection_surface_not_supported"),
							TEXT("This Blueprint semantic surface cannot be projected through the UMG importer."),
							TEXT("/semantic_type"), MakeShared<FJsonValueString>(Semantic),
							MakeShared<FJsonValueString>(TEXT("class-default property, graph collection, graph, node, pin default, node property, or edge"))),
							UnitId, TEXT("/semantic_type"));
					BlueprintUnitIds.Add(UnitId);
				}
				else WidgetUnitIds.Add(UnitId);
			}
			for (const FStructuralEdit& Edit : Projection.StructuralEdits)
			{
				if (!IsBlueprintGraphUnit(Edit.GetOwnerUnitId())
					&& !IsBlueprintMemberCollection(Edit.GetOwnerUnitId()))
				{
					const FSemanticUnit* Owner = DesiredModel->FindUnit(Edit.GetOwnerUnitId());
					if (Owner && Owner->GetUnitKind() == ESemanticUnitKind::StructureState
						&& (Owner->GetRole() == TEXT("binding_collection")
							|| Owner->GetRole() == TEXT("animation_collection"))) continue;
					auto Actual = MakeShared<FJsonObject>();
					Actual->SetBoolField(TEXT("owner_present"), Owner != nullptr);
					if (Owner)
					{
						Actual->SetStringField(TEXT("role"), Owner->GetRole());
						Actual->SetBoolField(TEXT("structure_state"), Owner->GetUnitKind() == ESemanticUnitKind::StructureState);
					}
					return Reject(WidgetFact(TEXT("umg_semantic_widget_collection_required"),
						TEXT("This structural edit requires a binding or animation StructureState collection owner."),
						TEXT("/role"), MakeShared<FJsonValueObject>(Actual),
						MakeShared<FJsonValueString>(TEXT("binding_collection or animation_collection StructureState"))),
						Edit.GetOwnerUnitId(), TEXT("/role"));
				}
				BlueprintStructuralEdits.Add(Edit);
			}
			if (!UeAgentBlueprintAuthoring::TryProjectSemanticProviderExtension(
				DesiredModel, BlueprintUnitIds, Projection.PropertyChangesByUnitId,
				BlueprintStructuralEdits, OutExtension, OutError, &Workspace, &CurrentModel.Get())) return false;
			if (!BlueprintUnitIds.IsEmpty() && OutExtension->Values.IsEmpty())
				return Fail(OutError, WidgetFact(TEXT("umg_blueprint_projection_unconsumed"),
					FString::Printf(TEXT("Projection of %d changed Blueprint units produced no provider fields."), BlueprintUnitIds.Num()),
					TEXT("semantic_projection.provider_field_count"), MakeShared<FJsonValueNumber>(OutExtension->Values.Num()),
					MakeShared<FJsonValueString>(TEXT("greater than zero")), Blueprint.GetPathName(), TEXT("native_result")));
			auto Widgets = MakeShared<FJsonObject>();
			auto FindRead = [&](const FSemanticUnit& Unit)
			{
				return Requests.FindByPredicate([&](const UeAgentAuthoringState::FReadRequest& Read)
				{
					const auto& SnapshotIdentity = Read.SnapshotIdentityOverride.IsSet()
						? Read.SnapshotIdentityOverride.GetValue() : Read.TargetIdentity;
					return SnapshotIdentity.CanonicalKey == Unit.GetTargetIdentityKey();
				});
			};
			// The common keyed-item resolver owns create/update/remove and restores captured bindings.
			for (const FString& UnitId : WidgetUnitIds)
			{
				const FSemanticUnit* Unit = DesiredModel->FindUnit(UnitId);
				if (!Unit) return Reject(WidgetFact(TEXT("umg_semantic_unit_missing"),
					TEXT("The widget unit is missing from the desired semantic model."), TEXT("/"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)), UnitId, TEXT("/"));
				if (Unit->GetUnitKind() == ESemanticUnitKind::PropertySet) continue;
				if (Unit->GetUnitKind() == ESemanticUnitKind::StructureState
					&& Unit->GetRole() == TEXT("binding_collection"))
				{
					const auto* Value = Unit->GetValues().Find(TEXT("bindings"));
					TSharedPtr<FJsonValue> JsonValue;
					UE::UAI::Contract::FDiagnostic ConversionError;
					if (!Value)
						return Reject(WidgetFact(TEXT("umg_semantic_bindings_invalid"), TEXT("The bindings collection value is missing."),
							TEXT("/bindings"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)), UnitId, TEXT("/bindings"));
					if (!StateSemantic::CanonicalJson::TryToJsonValue(**Value, JsonValue, ConversionError))
						return Reject(MoveTemp(ConversionError), UnitId, TEXT("/bindings"));
					if (JsonValue->Type != EJson::Object)
						return Reject(WidgetInput(TEXT("umg_semantic_bindings_invalid"), TEXT("The bindings collection must be an object."),
							TEXT("/bindings"), JsonValue, TEXT("object")), UnitId, TEXT("/bindings"));
					auto Bindings = MakeShared<FJsonObject>();
					TSet<FString> NativeTargets;
					for (const auto& Row : JsonValue->AsObject()->Values)
					{
						if (!Row.Value.IsValid() || Row.Value->Type != EJson::Object)
							return Reject(WidgetInput(TEXT("umg_semantic_binding_invalid"), TEXT("Each binding row must be an object."),
								FieldPath(TEXT("/bindings"), Row.Key), Row.Value, TEXT("object")), UnitId, FieldPath(TEXT("/bindings"), Row.Key), Row.Key);
						auto Binding = MakeShared<FJsonObject>(*Row.Value->AsObject());
						FString Operation, BoundId, WidgetName, PropertyName;
						Binding->TryGetStringField(TEXT("operation"), Operation);
						Binding->TryGetStringField(TEXT("unit_id"), BoundId);
						Binding->TryGetStringField(TEXT("widget_name"), WidgetName);
						Binding->TryGetStringField(TEXT("property_name"), PropertyName);
						if (Operation == TEXT("update") || Operation == TEXT("remove"))
						{
							const FSemanticUnit* Bound = CurrentModel->FindUnit(BoundId);
							const auto* Read = Bound ? FindRead(*Bound) : nullptr;
							if (!Bound || Bound->GetOwnerRef() != UnitId || !Read
								|| Bound->GetSemanticTypeRef() != UeAgentUmgAuthoring::BindingSemanticId
								|| WidgetName != Read->TargetIdentity.GetSelector(TEXT("widget_name"))
								|| PropertyName != Read->TargetIdentity.GetSelector(TEXT("property_name")))
							{
								auto Error = WidgetFact(TEXT("umg_semantic_binding_identity_conflict"), FString(),
									FieldPath(TEXT("/bindings"), Row.Key) + TEXT("/unit_id"),
									MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
								if (!Bound) Error.PublicMessage = TEXT("The binding row refers to no current semantic binding unit.");
								else if (Bound->GetOwnerRef() != UnitId) Error.PublicMessage = TEXT("The bound unit belongs to another binding collection.");
								else if (!Read)
								{
									Error.PublicMessage = TEXT("The bound binding has no captured native read.");
									Error.SourceKind = TEXT("native_result");
									Error.PropertyPath = Error.SemanticPointer = Error.Source.InternalJsonPath = TEXT("native_read.available");
								}
								else if (Bound->GetSemanticTypeRef() != UeAgentUmgAuthoring::BindingSemanticId)
								{
									Error.PublicMessage = TEXT("The bound unit is not a UMG binding semantic unit.");
									Error.ActualValue = MakeShared<FJsonValueString>(Bound->GetSemanticTypeRef());
									Error.ExpectedValue = MakeShared<FJsonValueString>(UeAgentUmgAuthoring::BindingSemanticId);
								}
								else
								{
									const bool bWidgetMismatch = WidgetName != Read->TargetIdentity.GetSelector(TEXT("widget_name"));
									const TCHAR* Field = bWidgetMismatch
										? TEXT("widget_name") : TEXT("property_name");
									Error = WidgetFact(TEXT("umg_semantic_binding_identity_conflict"),
										FString::Printf(TEXT("The binding row's %s does not match its captured binding."), Field),
										FieldPath(FieldPath(TEXT("/bindings"), Row.Key), Field),
										MakeShared<FJsonValueString>(bWidgetMismatch ? WidgetName : PropertyName),
										MakeShared<FJsonValueString>(Read->TargetIdentity.GetSelector(Field)));
								}
								Error.ActualEvidence = MakeShared<FJsonValueString>(BoundId);
								if (Bound) Error.InternalContext = Bound->GetOwnerRef();
								Error.ExpectedEvidence = MakeShared<FJsonValueString>(UnitId);
								return Reject(MoveTemp(Error), UnitId, FieldPath(TEXT("/bindings"), Row.Key), Row.Key);
							}
						}
						else if (Operation != TEXT("create") || !BoundId.IsEmpty())
						{
							auto Error = Operation != TEXT("create")
								? WidgetFact(TEXT("umg_semantic_binding_operation_invalid"), TEXT("A new binding row requires operation 'create'."),
									FieldPath(TEXT("/bindings"), Row.Key) + TEXT("/operation"),
									MakeShared<FJsonValueString>(Operation), MakeShared<FJsonValueString>(TEXT("create")))
								: WidgetFact(TEXT("umg_semantic_binding_operation_invalid"), TEXT("A created binding row must not bind an existing unit_id."),
									FieldPath(TEXT("/bindings"), Row.Key) + TEXT("/unit_id"),
									MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
							Error.ActualEvidence = MakeShared<FJsonValueString>(BoundId);
							return Reject(MoveTemp(Error), UnitId, FieldPath(TEXT("/bindings"), Row.Key), Row.Key);
						}
						const FString NativeTarget = FString::Printf(TEXT("%d:%s%s"), WidgetName.Len(), *WidgetName, *PropertyName);
						if (WidgetName.IsEmpty() || PropertyName.IsEmpty() || NativeTargets.Contains(NativeTarget))
						{
							const FString Path = FieldPath(TEXT("/bindings"), Row.Key);
							if (WidgetName.IsEmpty() || PropertyName.IsEmpty())
							{
								const TCHAR* Field = WidgetName.IsEmpty() ? TEXT("widget_name") : TEXT("property_name");
								return Reject(WidgetFact(TEXT("umg_semantic_binding_target_conflict"),
									FString::Printf(TEXT("The binding %s must not be empty."), Field), FieldPath(Path, Field),
									MakeShared<FJsonValueString>(TEXT("")), MakeShared<FJsonValueString>(TEXT("non-empty string"))),
									UnitId, FieldPath(Path, Field), Row.Key);
							}
							auto Actual = MakeShared<FJsonObject>();
							Actual->SetStringField(TEXT("widget_name"), WidgetName);
							Actual->SetStringField(TEXT("property_name"), PropertyName);
							return Reject(WidgetFact(TEXT("umg_semantic_binding_target_conflict"),
								TEXT("More than one binding row targets the same widget and property."), Path,
								MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(TEXT("unique widget/property pair"))),
								UnitId, Path, Row.Key);
						}
						NativeTargets.Add(NativeTarget);
						Binding->RemoveField(TEXT("unit_id")); Binding->RemoveField(TEXT("id"));
						Workspace.AttachInlinePropertySources(UnitId,
							FieldPath(TEXT("/bindings"), Row.Key), *Binding, FString());
						Bindings->SetObjectField(Row.Key, Binding);
					}
					OutExtension->SetObjectField(TEXT("bindings"), Bindings);
					continue;
				}
				if (Unit->GetUnitKind() == ESemanticUnitKind::StructureState
					&& Unit->GetRole() == TEXT("animation_collection"))
				{
					const auto* Value = Unit->GetValues().Find(TEXT("animations"));
					TSharedPtr<FJsonValue> JsonValue;
					UE::UAI::Contract::FDiagnostic ConversionError;
					if (!Value)
						return Reject(WidgetFact(TEXT("umg_semantic_animations_invalid"), TEXT("The animations collection value is missing."),
							TEXT("/animations"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)), UnitId, TEXT("/animations"));
					if (!StateSemantic::CanonicalJson::TryToJsonValue(**Value, JsonValue, ConversionError))
						return Reject(MoveTemp(ConversionError), UnitId, TEXT("/animations"));
					if (JsonValue->Type != EJson::Object)
						return Reject(WidgetInput(TEXT("umg_semantic_animations_invalid"), TEXT("The animations collection must be an object."),
							TEXT("/animations"), JsonValue, TEXT("object")), UnitId, TEXT("/animations"));
					auto Animations = MakeShared<FJsonObject>();
					for (const auto& Row : JsonValue->AsObject()->Values)
					{
						if (!Row.Value.IsValid() || Row.Value->Type != EJson::Object)
							return Reject(WidgetInput(TEXT("umg_semantic_animation_invalid"), TEXT("Each animation row must be an object."),
								FieldPath(TEXT("/animations"), Row.Key), Row.Value, TEXT("object")), UnitId, FieldPath(TEXT("/animations"), Row.Key), Row.Key);
						auto Animation = MakeShared<FJsonObject>(*Row.Value->AsObject());
						FString BoundId, Name, Operation;
						Animation->TryGetStringField(TEXT("unit_id"), BoundId);
						Animation->TryGetStringField(TEXT("name"), Name);
						Animation->TryGetStringField(TEXT("operation"), Operation);
						if (Operation == TEXT("update") || Operation == TEXT("remove"))
						{
							const FSemanticUnit* Bound = CurrentModel->FindUnit(BoundId);
							const auto* Read = Bound ? FindRead(*Bound) : nullptr;
							if (!Bound || Bound->GetOwnerRef() != UnitId || !Read
								|| Bound->GetSemanticTypeRef() != UeAgentUmgAuthoring::AnimationSemanticId
								|| (!Name.IsEmpty() && Name != Read->TargetIdentity.GetSelector(TEXT("animation_name"))))
							{
								auto Error = WidgetFact(TEXT("umg_semantic_animation_identity_conflict"), FString(),
									FieldPath(TEXT("/animations"), Row.Key) + TEXT("/unit_id"),
									MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
								if (!Bound) Error.PublicMessage = TEXT("The animation row refers to no current semantic animation unit.");
								else if (Bound->GetOwnerRef() != UnitId) Error.PublicMessage = TEXT("The bound unit belongs to another animation collection.");
								else if (!Read)
								{
									Error.PublicMessage = TEXT("The bound animation has no captured native read.");
									Error.SourceKind = TEXT("native_result");
									Error.PropertyPath = Error.SemanticPointer = Error.Source.InternalJsonPath = TEXT("native_read.available");
									}
								else if (Bound->GetSemanticTypeRef() != UeAgentUmgAuthoring::AnimationSemanticId)
								{
									Error.PublicMessage = TEXT("The bound unit is not a UMG animation semantic unit.");
									Error.ActualValue = MakeShared<FJsonValueString>(Bound->GetSemanticTypeRef());
									Error.ExpectedValue = MakeShared<FJsonValueString>(UeAgentUmgAuthoring::AnimationSemanticId);
								}
								else Error = WidgetFact(TEXT("umg_semantic_animation_identity_conflict"),
									TEXT("The animation name does not match its captured animation."),
									FieldPath(TEXT("/animations"), Row.Key) + TEXT("/name"), MakeShared<FJsonValueString>(Name),
									MakeShared<FJsonValueString>(Read->TargetIdentity.GetSelector(TEXT("animation_name"))));
								Error.ActualEvidence = MakeShared<FJsonValueString>(BoundId);
								if (Bound) Error.InternalContext = Bound->GetOwnerRef();
								Error.ExpectedEvidence = MakeShared<FJsonValueString>(UnitId);
								return Reject(MoveTemp(Error), UnitId, FieldPath(TEXT("/animations"), Row.Key), Row.Key);
							}
							Name = Read->TargetIdentity.GetSelector(TEXT("animation_name"));
						}
						else if (Operation != TEXT("create") || !BoundId.IsEmpty())
						{
							auto Error = Operation != TEXT("create")
								? WidgetFact(TEXT("umg_semantic_animation_operation_invalid"), TEXT("A new animation row requires operation 'create'."),
									FieldPath(TEXT("/animations"), Row.Key) + TEXT("/operation"),
									MakeShared<FJsonValueString>(Operation), MakeShared<FJsonValueString>(TEXT("create")))
								: WidgetFact(TEXT("umg_semantic_animation_operation_invalid"), TEXT("A created animation row must not bind an existing unit_id."),
									FieldPath(TEXT("/animations"), Row.Key) + TEXT("/unit_id"),
									MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
							Error.ActualEvidence = MakeShared<FJsonValueString>(BoundId);
							return Reject(MoveTemp(Error), UnitId, FieldPath(TEXT("/animations"), Row.Key), Row.Key);
						}
						else Name = Row.Key;
						if (Name.IsEmpty()) return Reject(WidgetFact(TEXT("umg_semantic_animation_identity_missing"),
							Operation == TEXT("create") ? TEXT("A created animation requires a non-empty map key.")
								: TEXT("The captured animation has no animation name."), FieldPath(TEXT("/animations"), Row.Key),
							MakeShared<FJsonValueString>(Name), MakeShared<FJsonValueString>(TEXT("non-empty animation name")),
							FString(), Operation == TEXT("create") ? TEXT("") : TEXT("native_result")),
							UnitId, FieldPath(TEXT("/animations"), Row.Key), Row.Key);
						Animation->SetStringField(TEXT("name"), Name);
						Animation->RemoveField(TEXT("unit_id")); Animation->RemoveField(TEXT("id"));
						Workspace.AttachInlinePropertySources(UnitId,
							TEXT("/animations/") + Row.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")),
							*Animation, TEXT("name"));
						Animations->SetObjectField(Name, Animation);
					}
					OutExtension->SetObjectField(TEXT("animations"), Animations);
					continue;
				}
				const auto* Value = Unit->GetValues().Find(TEXT("widgets"));
				const FSemanticUnit* BeforeUnit = CurrentModel->FindUnit(UnitId);
				const auto* BeforeValue = BeforeUnit ? BeforeUnit->GetValues().Find(TEXT("widgets")) : nullptr;
				if (!Value || !BeforeValue || Unit->GetUnitKind() != ESemanticUnitKind::StructureState)
				{
					auto Error = WidgetFact(TEXT("umg_semantic_surface_not_supported"), !Value
						? TEXT("This semantic surface has no desired widgets collection.")
						: !BeforeValue ? TEXT("The captured semantic surface has no widgets collection.")
						: TEXT("Widget collection projection requires a StructureState unit."),
						TEXT("/widgets"), MakeShared<FJsonValueString>(Unit->GetSemanticTypeRef()),
						MakeShared<FJsonValueString>(TEXT("desired and captured StructureState widgets collections")));
					if (!BeforeValue && Value) Error.SourceKind = TEXT("native_result");
					return Reject(MoveTemp(Error), UnitId, TEXT("/widgets"));
				}
				TSharedPtr<FJsonValue> DesiredJson, BeforeJson;
				UE::UAI::Contract::FDiagnostic ConversionError;
				if (!StateSemantic::CanonicalJson::TryToJsonValue(**Value, DesiredJson, ConversionError))
					return Reject(MoveTemp(ConversionError), UnitId, TEXT("/widgets"));
				if (!StateSemantic::CanonicalJson::TryToJsonValue(**BeforeValue, BeforeJson, ConversionError))
				{
					if (ConversionError.SourceKind.IsEmpty()) ConversionError.SourceKind = TEXT("native_result");
					return Reject(MoveTemp(ConversionError), UnitId, TEXT("/widgets"));
				}
				if (DesiredJson->Type != EJson::Object || BeforeJson->Type != EJson::Object)
					return Reject(WidgetInput(TEXT("umg_semantic_widgets_invalid"),
						DesiredJson->Type != EJson::Object ? TEXT("The desired widgets collection must be an object.")
							: TEXT("The captured widgets collection must be an object."), TEXT("/widgets"),
						DesiredJson->Type != EJson::Object ? DesiredJson : BeforeJson, TEXT("object"), FString(),
						DesiredJson->Type != EJson::Object ? TEXT("") : TEXT("native_result")), UnitId, TEXT("/widgets"));
				for (const auto& Row : DesiredJson->AsObject()->Values)
				{
					if (!Row.Value.IsValid() || Row.Value->Type != EJson::Object)
						return Reject(WidgetInput(TEXT("umg_semantic_widget_invalid"), TEXT("Each widget row must be an object."),
							FieldPath(TEXT("/widgets"), Row.Key), Row.Value, TEXT("object")), UnitId, FieldPath(TEXT("/widgets"), Row.Key), Row.Key);
					const auto* Before = BeforeJson->AsObject()->Values.Find(Row.Key);
					FString BeforeText, DesiredText;
					if (!Json::TryCanonicalize(Row.Value, DesiredText, ConversionError))
						return Reject(MoveTemp(ConversionError), UnitId, FieldPath(TEXT("/widgets"), Row.Key), Row.Key);
					if (Before && !Json::TryCanonicalize(*Before, BeforeText, ConversionError))
					{
						if (ConversionError.SourceKind.IsEmpty()) ConversionError.SourceKind = TEXT("native_result");
						return Reject(MoveTemp(ConversionError), UnitId, FieldPath(TEXT("/widgets"), Row.Key), Row.Key);
					}
					if (Before && BeforeText == DesiredText) continue;
					auto Widget = MakeShared<FJsonObject>(*Row.Value->AsObject());
					FString Operation, Binding;
					Widget->TryGetStringField(TEXT("operation"), Operation);
					Widget->TryGetStringField(TEXT("unit_id"), Binding);
					FString WidgetName = Row.Key;
					if (Operation == TEXT("update") || Operation == TEXT("remove"))
					{
						const FSemanticUnit* Bound = CurrentModel->FindUnit(Binding);
						const auto* Read = Bound ? FindRead(*Bound) : nullptr;
						if (!Before || !Bound || Bound->GetOwnerRef() != UnitId || !Read
							|| !Bound->GetSemanticTypeRef().EndsWith(TEXT(".umg.widget")))
						{
							auto Error = WidgetFact(TEXT("umg_semantic_widget_binding_missing"), FString(),
								FieldPath(TEXT("/widgets"), Row.Key) + TEXT("/unit_id"),
								MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
							if (!Before)
							{
								Error = WidgetFact(TEXT("umg_semantic_widget_binding_missing"),
									TEXT("Update or remove requires a widget row present in the captured collection."),
									FieldPath(TEXT("/widgets"), Row.Key) + TEXT("/operation"), MakeShared<FJsonValueString>(Operation),
									MakeShared<FJsonValueString>(TEXT("create for an uncaptured row")));
							}
							else if (!Bound) Error.PublicMessage = TEXT("The widget row refers to no current semantic widget unit.");
							else if (Bound->GetOwnerRef() != UnitId) Error.PublicMessage = TEXT("The bound widget belongs to another widget collection.");
							else if (!Read)
							{
								Error.PublicMessage = TEXT("The bound widget has no captured native read.");
								Error.SourceKind = TEXT("native_result");
								Error.PropertyPath = Error.SemanticPointer = Error.Source.InternalJsonPath = TEXT("native_read.available");
								}
							else
							{
								Error.PublicMessage = TEXT("The bound unit is not a UMG widget semantic unit.");
								Error.ActualValue = MakeShared<FJsonValueString>(Bound->GetSemanticTypeRef());
								Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("semantic type ending in .umg.widget"));
							}
							Error.ActualEvidence = MakeShared<FJsonValueString>(Binding);
							if (Bound) Error.InternalContext = Bound->GetOwnerRef();
							Error.ExpectedEvidence = MakeShared<FJsonValueString>(UnitId);
							return Reject(MoveTemp(Error), UnitId, FieldPath(TEXT("/widgets"), Row.Key), Row.Key);
						}
						WidgetName = Read->TargetIdentity.GetSelector(TEXT("widget_name"));
					}
					else if (Operation != TEXT("create") || Before)
					{
						return Reject(WidgetFact(TEXT("umg_semantic_widget_operation_invalid"), Operation != TEXT("create")
							? TEXT("The widget row requires operation create, update, or remove.")
							: TEXT("Create cannot replace a widget row already present in the captured collection."),
							FieldPath(TEXT("/widgets"), Row.Key) + TEXT("/operation"),
							MakeShared<FJsonValueString>(Operation), MakeShared<FJsonValueString>(Before ? TEXT("update or remove") : TEXT("create"))),
							UnitId, FieldPath(TEXT("/widgets"), Row.Key) + TEXT("/operation"), Row.Key);
					}
					Widget->RemoveField(TEXT("unit_id"));
					Widget->RemoveField(TEXT("id"));
					Widget->SetStringField(TEXT("name"), WidgetName);
					// The authored map key is the instance name; a creation row's
					// top-level name selects its class, not its generated member.
					Workspace.AttachInlinePropertySources(UnitId,
						TEXT("/widgets/") + Row.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")),
						*Widget, FString());
					Widgets->SetObjectField(TEXT("widget:") + WidgetName, Widget);
				}
			}
			for (const FString& UnitId : WidgetUnitIds)
			{
				const FSemanticUnit* Unit = DesiredModel->FindUnit(UnitId);
				if (!Unit || Unit->GetUnitKind() != ESemanticUnitKind::PropertySet) continue;
				const auto* Property = Projection.PropertyChangesByUnitId.Find(UnitId);
				const auto* Read = FindRead(*Unit);
				if (!Property || !Property->IsValid())
				{
					auto Error = WidgetFact(TEXT("umg_semantic_property_change_missing"),
						TEXT("The changed property unit has no projected property change."), TEXT("semantic_projection.property_change"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), FString(), TEXT("native_result"));
					if (Read)
					{
						const FString PropertyPath = Read->TargetIdentity.GetSelector(TEXT("property_path"));
						if (!PropertyPath.IsEmpty()) Error.PropertyPath = PropertyPath;
						Error.CandidateName = Read->TargetIdentity.GetSelector(TEXT("widget_name"));
					}
					return Reject(MoveTemp(Error), UnitId, TEXT("/"));
				}
				if (!Read) return Reject(WidgetFact(TEXT("umg_semantic_property_native_read_missing"),
					TEXT("The changed property unit has no captured native read."), TEXT("native_read.available"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), FString(), TEXT("native_result")),
					UnitId, TEXT("/"));
				const UWidget* WidgetObject = Cast<UWidget>(Read->RootObject);
				const UPanelSlot* Slot = Cast<UPanelSlot>(Read->RootObject);
				if (Slot) WidgetObject = Slot->Content;
				if (!WidgetObject || WidgetObject->GetTypedOuter<UWidgetTree>() != Blueprint.WidgetTree)
				{
					auto Error = WidgetFact(TEXT("umg_semantic_property_owner_not_supported"), !WidgetObject
						? TEXT("The property owner is neither a widget nor a slot containing a widget.")
						: TEXT("The property owner belongs to a different widget tree."),
						TEXT("native_read.owner"), MakeShared<FJsonValueString>(Read->RootObject
							? Read->RootObject->GetClass()->GetPathName() : TEXT("missing")),
						MakeShared<FJsonValueString>(TEXT("widget or occupied panel slot in the target WidgetTree")),
						FString(), TEXT("native_result"));
					if (WidgetObject) Error.CandidateName = WidgetObject->GetName();
					const FString PropertyPath = Read->TargetIdentity.GetSelector(TEXT("property_path"));
					if (!PropertyPath.IsEmpty()) Error.PropertyPath = PropertyPath;
					if (WidgetObject)
					{
						const UWidgetTree* ActualTree = WidgetObject->GetTypedOuter<UWidgetTree>();
						Error.ActualValue = MakeShared<FJsonValueString>(ActualTree ? ActualTree->GetPathName() : TEXT("missing"));
						Error.ExpectedValue = MakeShared<FJsonValueString>(Blueprint.WidgetTree ? Blueprint.WidgetTree->GetPathName() : TEXT("missing"));
					}
					return Reject(MoveTemp(Error), UnitId, TEXT("/"));
				}
				const FString Key = TEXT("widget:") + WidgetObject->GetName();
				const TSharedPtr<FJsonObject>* Existing = nullptr;
				TSharedPtr<FJsonObject> Widget;
				if (Widgets->TryGetObjectField(Key, Existing)) Widget = *Existing;
				else
				{
					Widget = MakeShared<FJsonObject>();
					Widget->SetStringField(TEXT("operation"), TEXT("update"));
					Widgets->SetObjectField(Key, Widget);
				}
				if (Widget->GetStringField(TEXT("operation")) == TEXT("remove"))
				{
					auto Error = WidgetFact(TEXT("umg_semantic_removed_widget_property_conflict"),
						TEXT("A property change cannot target a widget removed by the same import."), TEXT("/"),
						MakeShared<FJsonValueString>(TEXT("remove")), MakeShared<FJsonValueString>(TEXT("retained widget")));
					const FString PropertyPath = Read->TargetIdentity.GetSelector(TEXT("property_path"));
					if (!PropertyPath.IsEmpty()) Error.PropertyPath = PropertyPath;
					return Reject(MoveTemp(Error), UnitId, TEXT("/"), WidgetObject->GetName());
				}
				TSharedPtr<FJsonObject> PropertyOwner = Widget;
				if (Slot)
				{
					if (Widget->TryGetObjectField(TEXT("slot"), Existing)) PropertyOwner = *Existing;
					else { PropertyOwner = MakeShared<FJsonObject>(); Widget->SetObjectField(TEXT("slot"), PropertyOwner); }
				}
				const TArray<TSharedPtr<FJsonValue>>* BeforeProperties = nullptr;
				TArray<TSharedPtr<FJsonValue>> Properties;
				if (PropertyOwner->TryGetArrayField(TEXT("properties"), BeforeProperties)) Properties = *BeforeProperties;
				Properties.Add(*Property);
				PropertyOwner->SetArrayField(TEXT("properties"), MoveTemp(Properties));
			}
			if (!Widgets->Values.IsEmpty())
			{
				auto Tree = MakeShared<FJsonObject>();
				Tree->SetObjectField(TEXT("widgets"), Widgets);
				OutExtension->SetObjectField(TEXT("widget_tree"), Tree);
			}
			return true;
		}

		bool TryImportSemanticWorkspace(
			const FString& Target,
			const FResolvedDomainHjsonDocument& Resolved,
			FString& OutPayload,
			FWorkspaceError& OutError)
		{
			using namespace UE::UAI::Composition::HjsonImport;

			if (!Resolved.SemanticWorkspace.IsValid())
			{
				return Fail(OutError, WidgetFact(TEXT("umg_widget_semantic_workspace_missing"),
					TEXT("The resolved UMG semantic workspace is unavailable."), TEXT("semantic_workspace.available"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Target, TEXT("native_result")));
			}
			if (!EnsureAuthoringRegistered(OutError)) return false;
			const FString TargetPackage = PackagePath(Target);
			UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(Target);
			if (!Blueprint || Blueprint->GetClass() != UWidgetBlueprint::StaticClass())
			{
				return Fail(OutError, WidgetFact(TEXT("umg_widget_target_unresolved"),
					TEXT("The target could not be loaded as an exact WidgetBlueprint asset."), TEXT("target.class"),
					MakeShared<FJsonValueString>(Blueprint ? Blueprint->GetClass()->GetPathName() : TEXT("missing")),
					MakeShared<FJsonValueString>(WidgetBlueprintType), Target, TEXT("native_result")));
			}

			TArray<UeAgentAuthoringState::FReadRequest> Requests;
			UE::UAI::Contract::FDiagnostic CaptureError;
			if (!UeAgentUmgAuthoring::BuildAssetExportSnapshotReadRequests(
					*Blueprint, Requests, CaptureError))
			{
				OutError.SetFailure(CaptureError);
				return false;
			}
			SelectRootPropertyImportReadRequests(*Resolved.SemanticWorkspace, Requests);
			UeAgentAuthoringState::FCaptureResult Capture;
			if (!CapturePropertyImportState(*Resolved.SemanticWorkspace, Requests, Capture))
			{
				OutError.SetFailure(Capture.Diagnostic);
				return false;
			}

			ContextResource::FUeAgentResourceTracker Resources =
				MakeSemanticWorkspaceResourceTracker();
			StateSemantic::FUeAgentCanonicalSnapshotPtr CanonicalSnapshot;
			FAuthoringModelPtr Model;
			if (!TryBuildPropertyImportCurrentCapture(Resolved.SemanticWorkspace.Get(),
					TEXT("ue.widget_blueprint.asset"),
					TEXT("llm_authoring"),
					Capture.Snapshot,
					Resources,
					CanonicalSnapshot,
					Model,
					OutError))
			{
				return false;
			}

			Capture.Snapshot.ReleaseNativeValues();
			const FString TargetObjectPath = ObjectPath(Target);
			return TryCoordinateSemanticWorkspaceImport(
				Resolved.SemanticWorkspace.ToSharedRef(), Model.ToSharedRef(),
				CanonicalSnapshot.ToSharedRef(),
				BuildSemanticWorkspaceImportDescriptor(TEXT("umg_widget.semantic_workspace"), *Model),
				Blueprint->GetPathName(),
				Resources,
				{
					TEXT("umg-widget-domain-provider"),
					Target,
					TargetObjectPath,
					TEXT("umg-widget-semantic"),
					TEXT("umg_widget_semantic_prepare_failed"),
					TEXT("umg_widget_semantic_apply_failed"),
					TEXT("umg_widget_readback_payload_invalid"),
					TEXT("umg_widget_compile_not_settled"),
					[Target](TArray<UeAgentAuthoringState::FReadRequest>& Requests, UE::UAI::Contract::FDiagnostic& Error)
					{
						UWidgetBlueprint* Native = LoadWidgetBlueprint(Target);
						if (!Native)
						{
							Error = WidgetFact(TEXT("umg_widget_semantic_readback_target_unresolved"),
								TEXT("The WidgetBlueprint could not be loaded for semantic readback."), TEXT("target.available"),
								MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Target, TEXT("native_result"));
							return false;
						}
						return UeAgentUmgAuthoring::BuildAssetExportSnapshotReadRequests(*Native, Requests, Error);
					}, {}, &UeAgentBlueprintAuthoring::ProjectObservedNodePinDefaults},
				[TargetPackage, &Blueprint, &Requests, &Model](
					const FString& RequestId,
					const FPreparedAuthoringImport& Prepared,
					FSemanticWorkspaceChangeProjection&& Projection,
					FRootPropertyApplyOutcome& OutOutcome)
				{
					UeAgentUmgAuthoring::FAssetApplyRequest ApplyRequest;
					ApplyRequest.ProviderExtension = MakeShared<FJsonObject>();
					FWorkspaceError ProjectionError;
					if (!HasOnlyRootPropertyIntents(*Prepared.GetWorkspace()) && (!TryProjectWidgetChanges(Prepared.GetWorkspace()->GetModel(), Model.ToSharedRef(),
						Requests, *Blueprint, Projection, ApplyRequest.ProviderExtension, ProjectionError, Prepared.GetWorkspace().Get())
						|| !UE::UAI::DomainAdapters::Blueprint::Surfaces::TryNormalizeBlueprintCandidateBindings(
							TargetPackage, ApplyRequest.ProviderExtension, ProjectionError)))
					{
						OutOutcome.Diagnostic = ProjectionError.GetDiagnostic();
						OutOutcome.Phase = TEXT("semantic_projection");
						return false;
					}
					ApplyRequest.Operation =
						UeAgentApplyInputContract::EOperation::Update;
					ApplyRequest.AssetPath = TargetPackage;
					ApplyRequest.Properties = MoveTemp(Projection.RootObjectProperties);
					ApplyRequest.FinalReadback = OutOutcome.FinalReadback;
					ApplyRequest.ProviderExtension->SetBoolField(TEXT("compile"), true);
					ApplyRequest.ProviderExtension->SetBoolField(TEXT("save"), true);
					UeAgentUmgAuthoring::FAssetApplyResult ApplyResult;
					const bool bApplied = UeAgentUmgAuthoring::ApplyAsset(
						ApplyRequest, ApplyResult, RequestId);
					OutOutcome.bApplied = bApplied && ApplyResult.IsApplied();
					OutOutcome.Phase = MoveTemp(ApplyResult.Phase);
					OutOutcome.Diagnostic = MoveTemp(ApplyResult.Diagnostic);
					ApplyResult.Asset = nullptr;
					Blueprint = nullptr;
					return bApplied;
				},
				[&Target](FString& OutReadbackPayload, FString& OutRevision,
					FWorkspaceError& OutReadError)
				{
					return TryRead(
						Target, OutReadbackPayload, OutRevision, OutReadError);
				},
				OutPayload,
				OutError);
		}

		bool TryImport(
			const FString& Target,
			const TSharedPtr<FJsonObject>& Params,
			FString& OutPayload,
			FWorkspaceError& OutError)
		{
			FString CanonicalParamsJson;
			if (!TryCanonicalizeObject(Params, CanonicalParamsJson, OutError))
			{
				return false;
			}
			FResolvedDomainHjsonDocument Resolved;
			if (!TryDecodeResolvedDocument(
					Target,
					CanonicalParamsJson,
					FDomainHjsonDocumentContract(
						TEXT("umg"), SchemaId, WidgetBlueprintType),
					Resolved,
					OutError))
			{
				return false;
			}
			UE::UAI::HjsonWorkspace::DomainAuthoring::FScopedDomainHjsonDiagnosticSource DiagnosticSource(Resolved, OutError);
			if (Resolved.bSemanticWorkspaceDirectory)
			{
				return TryImportSemanticWorkspace(
					Target, Resolved, OutPayload, OutError);
			}
			const TSharedPtr<FJsonObject>* DocumentField = nullptr;
			if (!Resolved.Root.IsValid()
				|| !Resolved.Root->TryGetObjectField(
					TEXT("document"), DocumentField)
				|| !DocumentField || !DocumentField->IsValid())
			{
				return Fail(OutError, WidgetInput(TEXT("umg_widget_document_required"),
					TEXT("The UMG import requires a document object."), TEXT("/root/document"),
					Resolved.Root.IsValid() ? Resolved.Root->TryGetField(TEXT("document")) : nullptr, TEXT("object"), Target));
			}
			const TSharedPtr<FJsonObject> Document = *DocumentField;
			const FString& ExpectedRevision = Resolved.ExpectedRevision;

			FString OperationText;
			UeAgentApplyInputContract::EOperation Operation =
				UeAgentApplyInputContract::EOperation::Update;
			const TSharedPtr<FJsonObject>* DocumentTarget = nullptr;
			FString AssetPath;
			if (!Document->TryGetStringField(TEXT("operation"), OperationText) || !TryOperation(OperationText, Operation))
			{
				return Fail(OutError, WidgetInput(TEXT("umg_widget_document_identity_required"),
					TEXT("The document operation must be create, update, or upsert."), TEXT("/root/document/operation"),
					Document->TryGetField(TEXT("operation")), TEXT("create | update | upsert"), Target));
			}
			if (!Document->TryGetObjectField(TEXT("target"), DocumentTarget) || !DocumentTarget || !DocumentTarget->IsValid())
			{
				return Fail(OutError, WidgetInput(TEXT("umg_widget_document_identity_required"),
					TEXT("The document target must be an object."), TEXT("/root/document/target"),
					Document->TryGetField(TEXT("target")), TEXT("object"), Target));
			}
			if (!(*DocumentTarget)->TryGetStringField(TEXT("asset_path"), AssetPath))
			{
				return Fail(OutError, WidgetInput(TEXT("umg_widget_document_identity_required"),
					TEXT("The document target requires an asset_path string."), TEXT("/root/document/target/asset_path"),
					(*DocumentTarget)->TryGetField(TEXT("asset_path")), TEXT("string"), Target));
			}
			const FString TargetPackage = PackagePath(Target);
			if (PackagePath(AssetPath) != TargetPackage)
			{
				return Fail(OutError, WidgetFact(TEXT("umg_widget_target_identity_mismatch"),
					TEXT("The document asset_path does not match the import target."), TEXT("/root/document/target/asset_path"),
					MakeShared<FJsonValueString>(AssetPath), MakeShared<FJsonValueString>(TargetPackage), Target));
			}

			UObject* ExistingObject = StaticLoadObject(
				UObject::StaticClass(), nullptr, *ObjectPath(Target));
			UWidgetBlueprint* Existing = Cast<UWidgetBlueprint>(ExistingObject);
			if (ExistingObject && (!Existing
				|| Existing->GetClass() != UWidgetBlueprint::StaticClass()))
			{
				return Fail(OutError, WidgetFact(TEXT("umg_widget_target_type_unsupported"),
					TEXT("The existing target is not an exact WidgetBlueprint asset."), TEXT("target.class"),
					MakeShared<FJsonValueString>(ExistingObject->GetClass()->GetPathName()),
					MakeShared<FJsonValueString>(WidgetBlueprintType), Target, TEXT("native_result")));
			}
			FString BeforePayload;
			FString CurrentRevision = TEXT("missing");
			if (Existing && !TryRead(Target, BeforePayload, CurrentRevision, OutError))
			{
				return false;
			}
			if (ExpectedRevision != CurrentRevision)
			{
				auto Error = WidgetFact(TEXT("umg_widget_stale_revision"),
					TEXT("The WidgetBlueprint revision does not match expected_revision."), TEXT("/expected_revision"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Target, TEXT("command_input"));
				Error.ActualEvidence = MakeShared<FJsonValueString>(CurrentRevision);
				Error.ExpectedEvidence = MakeShared<FJsonValueString>(ExpectedRevision);
				return Fail(OutError, MoveTemp(Error));
			}
			if ((Operation == UeAgentApplyInputContract::EOperation::Create && Existing)
				|| (Operation == UeAgentApplyInputContract::EOperation::Update && !Existing))
			{
				return Fail(OutError, WidgetFact(Existing ? TEXT("umg_widget_target_already_exists") : TEXT("umg_widget_target_missing"),
					Existing ? TEXT("Create requires an absent WidgetBlueprint target.") : TEXT("Update requires an existing WidgetBlueprint target."),
					TEXT("/root/document/operation"), MakeShared<FJsonValueBoolean>(Existing != nullptr),
					MakeShared<FJsonValueBoolean>(Operation == UeAgentApplyInputContract::EOperation::Update), Target));
			}

			UeAgentUmgAuthoring::FAssetApplyRequest ApplyRequest;
			ApplyRequest.Operation = Operation;
			ApplyRequest.AssetPath = TargetPackage;
			(*DocumentTarget)->TryGetStringField(
				TEXT("source_asset_path"), ApplyRequest.SourceAssetPath);
			const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
			if (Document->TryGetArrayField(TEXT("properties"), Properties) && Properties)
			{
				ApplyRequest.Properties = *Properties;
			}
			const TSharedPtr<FJsonObject>* ProviderExtension = nullptr;
			if (Document->TryGetObjectField(TEXT("provider_extension"), ProviderExtension)
				&& ProviderExtension && ProviderExtension->IsValid())
			{
				ApplyRequest.ProviderExtension = *ProviderExtension;
			}
			else
			{
				ApplyRequest.ProviderExtension = MakeShared<FJsonObject>();
			}
			ApplyRequest.ProviderExtension->SetBoolField(TEXT("compile"), true);
			ApplyRequest.ProviderExtension->SetBoolField(TEXT("save"), true);
			if (!EnsureAuthoringRegistered(OutError)) return false;

			ApplyRequest.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure)
			{
				if (TryReadbackUmgImportResult(Target, OutPayload, OutError)) return true;
				Failure.Diagnostic = OutError.GetDiagnostic();
				return false;
			};
			Existing = nullptr;
			ExistingObject = nullptr;
			UeAgentUmgAuthoring::FAssetApplyResult ApplyResult;
			const FString RequestId = TEXT("umg-widget-")
				+ FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
			if (!UeAgentUmgAuthoring::ApplyAsset(ApplyRequest, ApplyResult, RequestId))
			{
				return Fail(OutError, MoveTemp(ApplyResult.Diagnostic));
			}

			ApplyResult.Asset = nullptr;
			return true;
		}

		bool TryExport(
			const FString& Target,
			const TSharedPtr<FJsonObject>& Params,
			FString& OutPayload,
			FWorkspaceError& OutError,
			FPreparedWorkspaceExport* PreparedExport)
		{
			FString WorkspacePath;
			if (!Params->TryGetStringField(TEXT("workspace_path"), WorkspacePath)
				|| WorkspacePath.IsEmpty())
			{
				return Fail(OutError, WidgetInput(TEXT("umg_widget_export_path_required"),
					TEXT("Export requires a non-empty workspace_path string."), TEXT("/workspace_path"),
					Params->TryGetField(TEXT("workspace_path")), TEXT("non-empty string"), Target, TEXT("command_input")));
			}
			UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(Target);
			if (!Blueprint || Blueprint->GetClass() != UWidgetBlueprint::StaticClass())
			{
				return Fail(OutError, WidgetFact(TEXT("umg_widget_target_unresolved"),
					TEXT("The target could not be loaded as an exact WidgetBlueprint asset."), TEXT("target.class"),
					MakeShared<FJsonValueString>(Blueprint ? Blueprint->GetClass()->GetPathName() : TEXT("missing")),
					MakeShared<FJsonValueString>(WidgetBlueprintType), Target, TEXT("native_result")));
			}
			if (!EnsureAuthoringRegistered(OutError)) return false;
			UeAgentExportWorkspaceWriter::FWorkspaceResult ExportResult;
			FString ExportError;
			if (!UeAgentUmgAuthoring::ExportAssetWorkspace(*Blueprint, WorkspacePath, TEXT("llm_authoring"), ExportResult, PreparedExport))
			{
				OutError.SetFailure(ExportResult.Diagnostic);
				return false;
			}
			const TSharedPtr<FJsonObject> Result = ExportResult.ToPublicJsonObject();
			return CanonicalObject(Result, OutPayload, OutError);
		}
	}

	bool TryExecuteContentGraph(
		const FString& CommandId,
		const FString& TargetIdentity,
		const FString& CanonicalParamsJson,
		FString& OutPayloadJson,
		bool& bOutReadOnly,
		FWorkspaceError& OutError,
		FPreparedWorkspaceExport* PreparedExport)
	{
		OutPayloadJson.Reset();
		bOutReadOnly = false;
		OutError.Reset();
		TSharedPtr<FJsonObject> Params;
		UE::UAI::Contract::FDiagnostic JsonError;
		if (!Json::TryParseObject(CanonicalParamsJson, Params, JsonError))
		{
			return Fail(OutError, MoveTemp(JsonError));
		}
		FString Target;
		if (!Params->TryGetStringField(TEXT("target"), Target)
			|| ObjectPath(Target) != ObjectPath(TargetIdentity))
		{
			return Fail(OutError, WidgetInput(TEXT("umg_widget_target_identity_mismatch"),
				TEXT("The target parameter must resolve to the routed WidgetBlueprint target."), TEXT("/target"),
				Params->TryGetField(TEXT("target")), ObjectPath(TargetIdentity), TargetIdentity, TEXT("command_input")));
		}
		if (CommandId == TEXT("content_graph.import_workspace"))
		{
			return TryImport(Target, Params, OutPayloadJson, OutError);
		}
		if (CommandId == TEXT("content_graph.export_workspace"))
		{
			bOutReadOnly = true;
			return TryExport(Target, Params, OutPayloadJson, OutError, PreparedExport);
		}
		if (CommandId.StartsWith(TEXT("content_graph.")))
		{
			FString Revision;
			bOutReadOnly = true;
			return TryRead(Target, OutPayloadJson, Revision, OutError);
		}
		return Fail(OutError, WidgetFact(TEXT("umg_widget_command_not_supported"),
			TEXT("This UMG adapter requires a content_graph command."), TEXT("/command_id"),
			MakeShared<FJsonValueString>(CommandId), MakeShared<FJsonValueString>(TEXT("content_graph.*")), Target, TEXT("command_input")));
	}
}

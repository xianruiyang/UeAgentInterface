#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectStructuredAssetMechanics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Dom/JsonValue.h"
#include "Dom/JsonObject.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Misc/ScopeExit.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringTypeDescriptors.h"
#include "Domains/Reflection/Authoring/UeAgentMeshMaterialRelations.h"

#include "Animation/Skeleton.h"
#include "Engine/DataTable.h"
#include "DataTableEditorUtils.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshLODSettings.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSocket.h"
#include "Materials/MaterialInterface.h"
#include "PhysicsEngine/BodySetup.h"
#include "SkinnedAssetCompiler.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

namespace UE::UAI::DomainAdapters::AssetDataSimpleEffect
{
	void ReplaceSkeletalMeshSettings(USkeletalMesh& Mesh, const TArray<FSkeletalMaterial>& Materials,
		USkeletalMeshLODSettings* LodSettings, const FBoxSphereBounds& ImportedBounds, USkeleton* Skeleton,
		const bool bApplyLodSettings, const TArray<int32>* CapturedMaterialIndices)
	{
		Mesh.Modify();
		if (CapturedMaterialIndices)
			UeAgentReflectionAuthoring::MeshMaterialRelations::ReindexSkeletalSlots(
				Mesh, Mesh.GetMaterials().Num(), *CapturedMaterialIndices);
		Mesh.SetMaterials(Materials);
		if (bApplyLodSettings) Mesh.SetLODSettings(LodSettings);
		Mesh.SetImportedBounds(ImportedBounds);
		Mesh.SetSkeleton(Skeleton);
		Mesh.PostEditChange();
		FSkinnedAssetCompilingManager::Get().FinishCompilation({&Mesh});
	}

	bool TryReplaceDataTableRows(UDataTable& Table, UScriptStruct* RowStruct,
        const TMap<FName, const uint8*>& Rows, ExecutionMutation::FExecutionMutationError& OutError)
    {
        auto Reject = [&](const FString& Reason, const FString& Field,
            const TSharedPtr<FJsonValue>& Actual, const FString& Expected)
        {
            OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("asset_data_data_table_rows_invalid"),
                Field, MakeShared<FJsonValueString>(Expected), Actual, TEXT("execution"), Reason);
            OutError.SourceKind = TEXT("native_result");
            OutError.TargetPath = Table.GetPathName();
            OutError.Source = {};
            OutError.SemanticPointer.Reset();
            return false;
        };
        if (!FDataTableEditorUtils::IsValidTableStruct(RowStruct))
            return Reject(TEXT("The row structure is not valid for a DataTable."), TEXT("row_struct"),
                RowStruct ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(RowStruct->GetPathName()))
                    : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()), TEXT("valid DataTable row structure"));
        for (const auto& Row : Rows)
        {
            if (Row.Key.IsNone() || !Row.Value)
            {
                const FString Name = Row.Key.ToString();
                FString EscapedName = Name.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
                return Reject(Row.Key.IsNone() ? TEXT("A DataTable row has an empty name.")
                    : TEXT("The prepared DataTable row has no value storage."),
                    TEXT("rows/") + EscapedName + (Row.Key.IsNone() ? TEXT("/name") : TEXT("/value")),
                    Row.Key.IsNone() ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Name))
                        : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()),
                    Row.Key.IsNone() ? TEXT("nonempty row name") : TEXT("available row storage"));
            }
        }
        Table.EmptyTable();
        Table.RowStruct = RowStruct;
        for (const auto& Row : Rows) Table.AddRow(Row.Key, Row.Value, RowStruct);
        return true;
    }

	using namespace ContextResource;
	using namespace ExecutionMutation;
	using namespace StateSemantic;

	namespace
	{

        FExecutionMutationError JsonFailure(const FString& Code, const FString& Reason, const FString& Field,
            const TSharedPtr<FJsonValue>& Actual, const TSharedPtr<FJsonValue>& Expected,
            const TCHAR* SourceKind = TEXT("authored_value"))
        {
            auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Field, Expected, Actual, TEXT("execution"), Reason);
            Error.SourceKind = SourceKind;
            Error.ActualValuePresent = Actual.IsValid();
            Error.ExpectedValuePresent = Expected.IsValid();
            if (Error.SourceKind == TEXT("native_result"))
            {
                Error.Source = {};
                Error.SemanticPointer.Reset();
            }
            return Error;
        }

        FExecutionMutationError ValueFailure(const FString& Code, const FString& Reason, const FString& Field,
            const FUeAgentCanonicalValue* Actual, const FString& Expected,
            const TCHAR* SourceKind = TEXT("authored_value"))
        {
            auto Error = JsonFailure(Code, Reason, Field, nullptr, MakeShared<FJsonValueString>(Expected), SourceKind);
            Error.ActualValuePresent = Actual != nullptr;
            Contract::FDiagnostic ProjectionError;
            if (Actual && !CanonicalJson::TryToJsonValue(*Actual, Error.ActualValue, ProjectionError))
            {
                ProjectionError.SourceKind = TEXT("native_result");
                Error.Diagnostics.Add(MoveTemp(ProjectionError));
            }
            return Error;
        }


        FExecutionMutationError NativeFailure(const FString& Code, const FString& Reason, const FString& Field,
            const TSharedPtr<FJsonValue>& Actual, const TSharedPtr<FJsonValue>& Expected)
        {
            auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Field, Expected, Actual, TEXT("execution"), Reason);
            Error.SourceKind = TEXT("native_result");
            Error.Source = {};
            Error.SemanticPointer.Reset();
            Error.ActualValuePresent = Actual.IsValid();
            Error.ExpectedValuePresent = Expected.IsValid();
            return Error;
        }

        TSharedPtr<FJsonValue> ObjectClassValue(const UObject* Object)
        {
            return Object ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Object->GetClass()->GetPathName()))
                : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>());
        }

		enum class EStructuredAssetKind : uint8
		{
			StaticMesh,
			DataTable,
			SkeletalMesh,
		};

		const EStructuredAssetKind* FindStructuredAssetKind(
			const FString& NativeMechanicId)
		{
			static const TMap<FString, EStructuredAssetKind> Kinds{
				{
					TEXT(
						"uai.asset_data.mechanic:"
						"update_static_mesh:v1"),
					EStructuredAssetKind::StaticMesh,
				},
				{
					TEXT(
						"uai.asset_data.mechanic:"
						"replace_data_table:v1"),
					EStructuredAssetKind::DataTable,
				},
				{
					TEXT(
						"uai.asset_data.mechanic:"
						"update_skeletal_mesh:v1"),
					EStructuredAssetKind::SkeletalMesh,
				},
			};
			return Kinds.Find(NativeMechanicId);
		}

		bool Fail(FExecutionMutationError& OutError, UE::UAI::Contract::FDiagnostic Diagnostic)
		{
			OutError = MoveTemp(Diagnostic);
			return false;
		}

        bool TryRequireChange(const FString& Field, const bool bPresent, const FString& Presence,
            const FUeAgentCanonicalValue* Value, const TCHAR* ExpectedPresence, FExecutionMutationError& OutError)
        {
            if (!bPresent)
                return Fail(OutError, ValueFailure(TEXT("asset_data_structured_asset_change_missing"),
                    TEXT("A required field change is missing."), Field, nullptr, TEXT("field change")));
            if (Presence != ExpectedPresence)
                return Fail(OutError, JsonFailure(TEXT("asset_data_structured_asset_presence_invalid"),
                    TEXT("The field requires a different operation."), Field + TEXT("/presence"),
                    MakeShared<FJsonValueString>(Presence), MakeShared<FJsonValueString>(ExpectedPresence)));
            if (!Value)
                return Fail(OutError, ValueFailure(TEXT("asset_data_structured_asset_value_missing"),
                    TEXT("The field operation requires a value."), Field, nullptr, TEXT("operation value")));
            return true;
        }

        FExecutionMutationError UnsupportedMechanic(const FString& NativeMechanicId)
        {
            auto Error = NativeFailure(TEXT("asset_data_structured_asset_projection_unsupported"),
                TEXT("The selected native mechanic does not support structured asset operations."), TEXT("native_mechanic"),
                nullptr, MakeShared<FJsonValueString>(TEXT("registered structured asset mechanic")));
            Error.InternalContext = NativeMechanicId;
            return Error;
        }

        bool TryResolveStructuredReference(IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
            const FString& Ref, UObject*& Out, const FString& Field, FExecutionMutationError& OutError)
        {
            if (Resolver.TryResolveExisting(Ref, Out, OutError)) return true;
            if (OutError.PropertyPath.IsEmpty()) OutError.PropertyPath = Field;
            return false;
        }

        FExecutionMutationError ReferenceClassFailure(const FString& Field, const FString& Ref,
            const UObject* Object, const UClass& ExpectedClass)
        {
            auto Actual = MakeShared<FJsonObject>();
            Actual->SetStringField(TEXT("reference"), Ref);
            Actual->SetField(TEXT("resolved_class"), ObjectClassValue(Object));
            return JsonFailure(TEXT("asset_data_structured_reference_type_invalid"),
                TEXT("The reference did not resolve to the required asset class."), Field,
                MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(ExpectedClass.GetPathName()));
        }

		const FStructuredAssetChangeView* FindChange(
			const TArray<FStructuredAssetChangeView>& Changes,
			const FString& FieldId)
		{
			for (const FStructuredAssetChangeView& Change : Changes)
			{
				if (Change.FieldId == FieldId)
				{
					return &Change;
				}
			}
			return nullptr;
		}

		const FUeAgentCanonicalValue* FindObjectField(
			const FUeAgentCanonicalValue& Object,
			const FString& Field)
		{
			if (Object.GetKind() != EUeAgentValueKind::Object)
			{
				return nullptr;
			}
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Item
				: Object.GetObjectItems())
			{
				if (Item.Key == Field)
				{
					return &Item.Value.Get();
				}
			}
			return nullptr;
		}

		const FUeAgentCanonicalValueRef* FindObjectFieldRef(
			const FUeAgentCanonicalValue& Object,
			const FString& Field)
		{
			if (Object.GetKind() != EUeAgentValueKind::Object)
			{
				return nullptr;
			}
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Item
				: Object.GetObjectItems())
			{
				if (Item.Key == Field)
				{
					return &Item.Value;
				}
			}
			return nullptr;
		}

		bool HasExactObjectFields(
			const FUeAgentCanonicalValue& Object,
			std::initializer_list<const TCHAR*> Names)
		{
			if (Object.GetKind() != EUeAgentValueKind::Object
				|| Object.GetObjectItems().Num()
					!= static_cast<int32>(Names.size()))
			{
				return false;
			}
			for (const TCHAR* Name : Names)
			{
				if (!FindObjectField(Object, Name))
				{
					return false;
				}
			}
			return true;
		}

        FString ChildPath(const FString& Parent, const FString& Name)
        {
            return Parent + TEXT("/") + Name.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
        }

        bool TryRequireObjectFields(const FUeAgentCanonicalValue& Value,
            std::initializer_list<const TCHAR*> Names, const FString& Field, FExecutionMutationError& OutError)
        {
            if (HasExactObjectFields(Value, Names)) return true;
            if (Value.GetKind() != EUeAgentValueKind::Object)
                return Fail(OutError, ValueFailure(TEXT("asset_data_structured_object_invalid"),
                    TEXT("An object with the required fields is required."), Field, &Value, TEXT("object")));
            TArray<TSharedPtr<FJsonValue>> ActualNames;
            TArray<TSharedPtr<FJsonValue>> ExpectedNames;
            for (const auto& Item : Value.GetObjectItems()) ActualNames.Add(MakeShared<FJsonValueString>(Item.Key));
            for (const TCHAR* Name : Names) ExpectedNames.Add(MakeShared<FJsonValueString>(Name));
            for (const TCHAR* Name : Names)
                if (!FindObjectField(Value, Name))
                    return Fail(OutError, JsonFailure(TEXT("asset_data_structured_field_missing"),
                        TEXT("A required object field is missing."), ChildPath(Field, Name), nullptr,
                        MakeShared<FJsonValueString>(TEXT("present field"))));
            return Fail(OutError, JsonFailure(TEXT("asset_data_structured_object_fields_invalid"),
                TEXT("The object must contain exactly the supported fields."), Field,
                MakeShared<FJsonValueArray>(ActualNames), MakeShared<FJsonValueArray>(ExpectedNames)));
        }

        bool TryReadString(const FUeAgentCanonicalValue& Value, FString& Out,
            const FString& Field, FExecutionMutationError& OutError)
        {
            if (Value.GetKind() != EUeAgentValueKind::String)
                return Fail(OutError, ValueFailure(TEXT("asset_data_structured_string_invalid"),
                    TEXT("A string is required."), Field, &Value, TEXT("string")));
            Out = Value.GetText();
            return true;
        }

        bool TryReadBool(const FUeAgentCanonicalValue& Value, bool& Out,
            const FString& Field, FExecutionMutationError& OutError)
        {
            if (Value.GetKind() != EUeAgentValueKind::Boolean)
                return Fail(OutError, ValueFailure(TEXT("asset_data_structured_boolean_invalid"),
                    TEXT("A boolean is required."), Field, &Value, TEXT("boolean")));
            Out = Value.GetBoolean();
            return true;
        }

        bool TryReadNumber(const FUeAgentCanonicalValue& Value, double& Out,
            const FString& Field, FExecutionMutationError& OutError)
        {
            if (Value.GetKind() != EUeAgentValueKind::Integer && Value.GetKind() != EUeAgentValueKind::Decimal)
                return Fail(OutError, ValueFailure(TEXT("asset_data_structured_number_invalid"),
                    TEXT("A numeric value is required."), Field, &Value, TEXT("finite number")));
            if (!TryParseFiniteDecimal(Value.GetLexeme(), Out))
                return Fail(OutError, ValueFailure(TEXT("asset_data_structured_number_invalid"),
                    TEXT("The number cannot be represented as a finite native value."), Field, &Value, TEXT("finite number")));
            return true;
        }

        bool TryReadInt32(const FUeAgentCanonicalValue& Value, int32& Out,
            const FString& Field, FExecutionMutationError& OutError)
        {
            int64 Number = 0;
            if ((Value.GetKind() != EUeAgentValueKind::Integer && Value.GetKind() != EUeAgentValueKind::Decimal)
                || !TryParseInt64Number(Value.GetLexeme(), Number) || Number < MIN_int32 || Number > MAX_int32)
                return Fail(OutError, ValueFailure(TEXT("asset_data_structured_int32_invalid"),
                    TEXT("The value must be an integer representable as int32."), Field, &Value,
                    TEXT("integer in [-2147483648, 2147483647]")));
            Out = static_cast<int32>(Number);
            return true;
        }

        bool TryReadVector(const FUeAgentCanonicalValue& Value, FVector& Out,
            const FString& Field, FExecutionMutationError& OutError)
        {
            if (!TryRequireObjectFields(Value, {TEXT("x"), TEXT("y"), TEXT("z")}, Field, OutError)) return false;
            double X = 0.0, Y = 0.0, Z = 0.0;
            if (!TryReadNumber(*FindObjectField(Value, TEXT("x")), X, Field + TEXT("/x"), OutError)
                || !TryReadNumber(*FindObjectField(Value, TEXT("y")), Y, Field + TEXT("/y"), OutError)
                || !TryReadNumber(*FindObjectField(Value, TEXT("z")), Z, Field + TEXT("/z"), OutError)) return false;
            Out = FVector(X, Y, Z);
            if (Out.ContainsNaN())
                return Fail(OutError, ValueFailure(TEXT("asset_data_structured_vector_invalid"),
                    TEXT("The vector cannot be represented by finite native components."), Field, &Value, TEXT("finite vector")));
            return true;
        }

        bool TryReadRotator(const FUeAgentCanonicalValue& Value, FRotator& Out,
            const FString& Field, FExecutionMutationError& OutError)
        {
            if (!TryRequireObjectFields(Value, {TEXT("pitch"), TEXT("yaw"), TEXT("roll")}, Field, OutError)) return false;
            double Pitch = 0.0, Yaw = 0.0, Roll = 0.0;
            if (!TryReadNumber(*FindObjectField(Value, TEXT("pitch")), Pitch, Field + TEXT("/pitch"), OutError)
                || !TryReadNumber(*FindObjectField(Value, TEXT("yaw")), Yaw, Field + TEXT("/yaw"), OutError)
                || !TryReadNumber(*FindObjectField(Value, TEXT("roll")), Roll, Field + TEXT("/roll"), OutError)) return false;
            Out = FRotator(Pitch, Yaw, Roll);
            if (Out.ContainsNaN())
                return Fail(OutError, ValueFailure(TEXT("asset_data_structured_rotator_invalid"),
                    TEXT("The rotation cannot be represented by finite native components."), Field, &Value, TEXT("finite rotator")));
            return true;
        }

		struct FMaterialSlotValue final
		{
			FName SlotName;
			TWeakObjectPtr<UMaterialInterface> Material;
		};

        bool TryReadMaterialSlots(IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
            const FUeAgentCanonicalValue& Value, TArray<FMaterialSlotValue>& Out, FExecutionMutationError& OutError)
        {
            if (Value.GetKind() != EUeAgentValueKind::Array)
                return Fail(OutError, ValueFailure(TEXT("asset_data_material_slots_invalid"),
                    TEXT("Material slots must be an array."), TEXT("materials"), &Value, TEXT("array")));
            Out.Reset();
            TSet<FName> Seen;
            for (int32 Index = 0; Index < Value.GetArrayItems().Num(); ++Index)
            {
                const auto& Item = Value.GetArrayItems()[Index];
                const FString Field = TEXT("materials/") + FString::FromInt(Index);
                if (!TryRequireObjectFields(*Item, {TEXT("slot_name"), TEXT("material_ref")}, Field, OutError)) return false;
                FString SlotName, MaterialRef;
                if (!TryReadString(*FindObjectField(*Item, TEXT("slot_name")), SlotName, Field + TEXT("/slot_name"), OutError)
                    || !TryReadString(*FindObjectField(*Item, TEXT("material_ref")), MaterialRef, Field + TEXT("/material_ref"), OutError)) return false;
                if (SlotName.TrimStartAndEnd().IsEmpty())
                    return Fail(OutError, JsonFailure(TEXT("asset_data_material_slot_invalid"),
                        TEXT("The material slot name must not be blank."), Field + TEXT("/slot_name"),
                        MakeShared<FJsonValueString>(SlotName), MakeShared<FJsonValueString>(TEXT("nonblank slot name"))));
                FMaterialSlotValue Slot;
                Slot.SlotName = FName(*SlotName);
                if (Seen.Contains(Slot.SlotName))
                    return Fail(OutError, JsonFailure(TEXT("asset_data_material_slot_duplicate"),
                        TEXT("The material slot name is duplicated."), Field + TEXT("/slot_name"),
                        MakeShared<FJsonValueString>(SlotName), MakeShared<FJsonValueString>(TEXT("unique slot name"))));
                UObject* Resolved = nullptr;
                if (!Resolver.TryResolveExisting(MaterialRef, Resolved, OutError))
                {
                    if (OutError.PropertyPath.IsEmpty()) OutError.PropertyPath = Field + TEXT("/material_ref");
                    return false;
                }
                UMaterialInterface* Material = Cast<UMaterialInterface>(Resolved);
                if (!Material)
                {
                    auto Actual = MakeShared<FJsonObject>();
                    Actual->SetStringField(TEXT("reference"), MaterialRef);
                    Actual->SetField(TEXT("resolved_class"), ObjectClassValue(Resolved));
                    return Fail(OutError, JsonFailure(TEXT("asset_data_material_ref_invalid"),
                        TEXT("The material reference did not resolve to a material interface."), Field + TEXT("/material_ref"),
                        MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(UMaterialInterface::StaticClass()->GetPathName())));
                }
                Slot.Material = Material;
                Seen.Add(Slot.SlotName);
                Out.Add(MoveTemp(Slot));
            }
            return true;
        }

		TArray<FStaticMaterial> MakeStaticMaterials(
			const TArray<FMaterialSlotValue>& Slots)
		{
			TArray<FStaticMaterial> Out;
			Out.Reserve(Slots.Num());
			for (const FMaterialSlotValue& Slot : Slots)
			{
				Out.Emplace(
					Slot.Material.Get(),
					Slot.SlotName,
					Slot.SlotName);
			}
			return Out;
		}

		TArray<FSkeletalMaterial> MakeSkeletalMaterials(
			const TArray<FMaterialSlotValue>& Slots)
		{
			TArray<FSkeletalMaterial> Out;
			Out.Reserve(Slots.Num());
			for (const FMaterialSlotValue& Slot : Slots)
			{
				Out.Emplace(
					Slot.Material.Get(),
					Slot.SlotName,
					Slot.SlotName);
			}
			return Out;
		}

		TArray<FMaterialSlotValue> CaptureStaticMaterialSlots(
			const UStaticMesh& Mesh)
		{
			TArray<FMaterialSlotValue> Out;
			for (const FStaticMaterial& Material
				: Mesh.GetStaticMaterials())
			{
				FMaterialSlotValue Slot;
				Slot.SlotName = Material.MaterialSlotName;
				Slot.Material = Material.MaterialInterface;
				Out.Add(MoveTemp(Slot));
			}
			return Out;
		}

        bool TryReadRemovalNames(const FUeAgentCanonicalValue& Value, TSet<FName>& Out,
            FExecutionMutationError& OutError, const FString& ErrorCode, const FString& Field)
        {
            if (Value.GetKind() != EUeAgentValueKind::Array)
                return Fail(OutError, ValueFailure(ErrorCode, TEXT("Names to remove must be an array."), Field, &Value, TEXT("array of nonblank names")));
            Out.Reset();
            for (int32 Index = 0; Index < Value.GetArrayItems().Num(); ++Index)
            {
                const auto& Item = Value.GetArrayItems()[Index];
                const FString Path = Field + TEXT("/") + FString::FromInt(Index);
                FString Name;
                if (!TryReadString(*Item, Name, Path, OutError)) return false;
                if (Name.TrimStartAndEnd().IsEmpty())
                    return Fail(OutError, ValueFailure(ErrorCode, TEXT("A name to remove must not be blank."), Path, &Item.Get(), TEXT("nonblank name")));
                Out.Add(FName(*Name));
            }
            return true;
        }

		bool TryApplyMaterialPresence(
			IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
			const FStructuredAssetChangeView& Change,
			const TArray<FMaterialSlotValue>& Current,
			TArray<FMaterialSlotValue>& Out,
			FExecutionMutationError& OutError)
		{
			if (Change.Presence == TEXT("clear"))
			{
				Out.Reset();
				return true;
			}
            if (!Change.CanonicalValue)
                return Fail(OutError, ValueFailure(TEXT("asset_data_material_slots_invalid"),
                    TEXT("The material operation requires a value."), TEXT("materials"), nullptr, TEXT("material operation value")));
			if (Change.Presence == TEXT("replace"))
			{
				return TryReadMaterialSlots(
					Resolver,
					*Change.CanonicalValue,
					Out,
					OutError);
			}
			if (Change.Presence == TEXT("set"))
			{
				TArray<FMaterialSlotValue> Updates;
				if (!TryReadMaterialSlots(
						Resolver,
						*Change.CanonicalValue,
						Updates,
						OutError))
				{
					return false;
				}
				Out = Current;
				for (FMaterialSlotValue& Update : Updates)
				{
					const int32 Existing =
						Out.IndexOfByPredicate(
							[&Update](
								const FMaterialSlotValue& Item)
							{
								return Item.SlotName
									== Update.SlotName;
							});
					if (Existing == INDEX_NONE)
					{
						Out.Add(MoveTemp(Update));
					}
					else
					{
						Out[Existing] = MoveTemp(Update);
					}
				}
				return true;
			}
			if (Change.Presence == TEXT("remove"))
			{
				TSet<FName> Names;
				if (!TryReadRemovalNames(
						*Change.CanonicalValue,
						Names,
						OutError,
						TEXT(
							"asset_data_material_remove_invalid"), TEXT("materials")))
				{
					return false;
				}
				Out = Current;
				Out.RemoveAll(
					[&Names](const FMaterialSlotValue& Item)
					{
						return Names.Contains(Item.SlotName);
					});
				return true;
			}
            return Fail(OutError, JsonFailure(TEXT("asset_data_material_presence_invalid"),
                TEXT("The material operation is not supported."), TEXT("materials/presence"),
                MakeShared<FJsonValueString>(Change.Presence), MakeShared<FJsonValueString>(TEXT("clear, set, replace, or remove"))));
		}

		bool TryReadCollisionTraceMode(
			const FString& Value,
			ECollisionTraceFlag& Out, FExecutionMutationError& OutError)
		{
			const FString Name = Value.TrimStartAndEnd().ToLower();
			if (Name == TEXT("default"))
			{
				Out = CTF_UseDefault;
			}
			else if (Name == TEXT("use_simple_and_complex"))
			{
				Out = CTF_UseSimpleAndComplex;
			}
			else if (Name == TEXT("use_simple_as_complex"))
			{
				Out = CTF_UseSimpleAsComplex;
			}
			else if (Name == TEXT("use_complex_as_simple"))
			{
				Out = CTF_UseComplexAsSimple;
			}
			else
			{
                return Fail(OutError, JsonFailure(TEXT("asset_data_collision_trace_invalid"),
                    TEXT("The collision trace mode is not supported."), TEXT("collision/trace_mode"),
                    MakeShared<FJsonValueString>(Value), MakeShared<FJsonValueString>(TEXT("default, use_simple_and_complex, use_simple_as_complex, or use_complex_as_simple"))));
			}
			return true;
		}

        bool TryReadCollision(const FUeAgentCanonicalValue& Value, FKAggregateGeom& OutGeometry,
            ECollisionTraceFlag& OutTrace, FExecutionMutationError& OutError)
        {
            if (!TryRequireObjectFields(Value, {TEXT("trace_mode"), TEXT("boxes"), TEXT("spheres"), TEXT("capsules")},
                TEXT("collision"), OutError)) return false;
            FString TraceMode;
            if (!TryReadString(*FindObjectField(Value, TEXT("trace_mode")), TraceMode, TEXT("collision/trace_mode"), OutError)
                || !TryReadCollisionTraceMode(TraceMode, OutTrace, OutError)) return false;
            const FUeAgentCanonicalValue* Boxes = FindObjectField(Value, TEXT("boxes"));
            const FUeAgentCanonicalValue* Spheres = FindObjectField(Value, TEXT("spheres"));
            const FUeAgentCanonicalValue* Capsules = FindObjectField(Value, TEXT("capsules"));
            const TCHAR* Names[] = {TEXT("boxes"), TEXT("spheres"), TEXT("capsules")};
            const FUeAgentCanonicalValue* Arrays[] = {Boxes, Spheres, Capsules};
            for (int32 Index = 0; Index < 3; ++Index)
                if (!Arrays[Index] || Arrays[Index]->GetKind() != EUeAgentValueKind::Array)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_collision_invalid"),
                        TEXT("Collision shapes must be an array."), FString(TEXT("collision/")) + Names[Index], Arrays[Index], TEXT("array")));
            OutGeometry = FKAggregateGeom();
            for (int32 Index = 0; Index < Boxes->GetArrayItems().Num(); ++Index)
            {
                const auto& Item = Boxes->GetArrayItems()[Index];
                const FString Field = TEXT("collision/boxes/") + FString::FromInt(Index);
                if (!TryRequireObjectFields(*Item, {TEXT("center"), TEXT("extent"), TEXT("rotation")}, Field, OutError)) return false;
                FVector Center = FVector::ZeroVector, Extent = FVector::ZeroVector;
                FRotator Rotation = FRotator::ZeroRotator;
                if (!TryReadVector(*FindObjectField(*Item, TEXT("center")), Center, Field + TEXT("/center"), OutError)
                    || !TryReadVector(*FindObjectField(*Item, TEXT("extent")), Extent, Field + TEXT("/extent"), OutError)
                    || !TryReadRotator(*FindObjectField(*Item, TEXT("rotation")), Rotation, Field + TEXT("/rotation"), OutError)) return false;
                if (Extent.GetMin() <= 0.0)
                {
                    const TCHAR* Axis = Extent.X <= 0.0 ? TEXT("x") : Extent.Y <= 0.0 ? TEXT("y") : TEXT("z");
                    return Fail(OutError, ValueFailure(TEXT("asset_data_collision_box_invalid"),
                        TEXT("The box extent component must be positive."), Field + TEXT("/extent/") + Axis,
                        FindObjectField(*FindObjectField(*Item, TEXT("extent")), Axis), TEXT("number > 0")));
                }
                FKBoxElem Box;
                Box.Center = Center;
                Box.Rotation = Rotation;
                Box.X = Extent.X * 2.0;
                Box.Y = Extent.Y * 2.0;
                Box.Z = Extent.Z * 2.0;
                OutGeometry.BoxElems.Add(Box);
            }
            for (int32 Index = 0; Index < Spheres->GetArrayItems().Num(); ++Index)
            {
                const auto& Item = Spheres->GetArrayItems()[Index];
                const FString Field = TEXT("collision/spheres/") + FString::FromInt(Index);
                if (!TryRequireObjectFields(*Item, {TEXT("center"), TEXT("radius")}, Field, OutError)) return false;
                FVector Center = FVector::ZeroVector;
                double Radius = 0.0;
                if (!TryReadVector(*FindObjectField(*Item, TEXT("center")), Center, Field + TEXT("/center"), OutError)
                    || !TryReadNumber(*FindObjectField(*Item, TEXT("radius")), Radius, Field + TEXT("/radius"), OutError)) return false;
                if (Radius <= 0.0)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_collision_sphere_invalid"),
                        TEXT("The sphere radius must be positive."), Field + TEXT("/radius"), FindObjectField(*Item, TEXT("radius")), TEXT("number > 0")));
                FKSphereElem Sphere;
                Sphere.Center = Center;
                Sphere.Radius = Radius;
                OutGeometry.SphereElems.Add(Sphere);
            }
            for (int32 Index = 0; Index < Capsules->GetArrayItems().Num(); ++Index)
            {
                const auto& Item = Capsules->GetArrayItems()[Index];
                const FString Field = TEXT("collision/capsules/") + FString::FromInt(Index);
                if (!TryRequireObjectFields(*Item, {TEXT("center"), TEXT("rotation"), TEXT("radius"), TEXT("length")}, Field, OutError)) return false;
                FVector Center = FVector::ZeroVector;
                FRotator Rotation = FRotator::ZeroRotator;
                double Radius = 0.0, Length = 0.0;
                if (!TryReadVector(*FindObjectField(*Item, TEXT("center")), Center, Field + TEXT("/center"), OutError)
                    || !TryReadRotator(*FindObjectField(*Item, TEXT("rotation")), Rotation, Field + TEXT("/rotation"), OutError)
                    || !TryReadNumber(*FindObjectField(*Item, TEXT("radius")), Radius, Field + TEXT("/radius"), OutError)
                    || !TryReadNumber(*FindObjectField(*Item, TEXT("length")), Length, Field + TEXT("/length"), OutError)) return false;
                if (Radius <= 0.0 || Length <= 0.0)
                {
                    const TCHAR* Component = Radius <= 0.0 ? TEXT("radius") : TEXT("length");
                    return Fail(OutError, ValueFailure(TEXT("asset_data_collision_capsule_invalid"),
                        TEXT("The capsule dimension must be positive."), Field + TEXT("/") + Component,
                        FindObjectField(*Item, Component), TEXT("number > 0")));
                }
                FKSphylElem Capsule;
                Capsule.Center = Center;
                Capsule.Rotation = Rotation;
                Capsule.Radius = Radius;
                Capsule.Length = Length;
                OutGeometry.SphylElems.Add(Capsule);
            }
            return true;
        }

		struct FSocketValue final
		{
			FName Name;
			FVector Location = FVector::ZeroVector;
			FRotator Rotation = FRotator::ZeroRotator;
			FVector Scale = FVector::OneVector;
		};

		TArray<FSocketValue> CaptureSockets(
			const UStaticMesh& Mesh)
		{
			TArray<FSocketValue> Out;
			for (const UStaticMeshSocket* Socket : Mesh.Sockets)
			{
				if (!Socket)
				{
					continue;
				}
				FSocketValue Value;
				Value.Name = Socket->SocketName;
				Value.Location = Socket->RelativeLocation;
				Value.Rotation = Socket->RelativeRotation;
				Value.Scale = Socket->RelativeScale;
				Out.Add(MoveTemp(Value));
			}
			return Out;
		}

        bool TryReadSockets(const FUeAgentCanonicalValue& Value,
            TArray<FSocketValue>& Out, FExecutionMutationError& OutError)
        {
            if (Value.GetKind() != EUeAgentValueKind::Array)
                return Fail(OutError, ValueFailure(TEXT("asset_data_sockets_invalid"),
                    TEXT("Sockets must be an array."), TEXT("sockets"), &Value, TEXT("array")));
            Out.Reset();
            TSet<FName> Seen;
            for (int32 Index = 0; Index < Value.GetArrayItems().Num(); ++Index)
            {
                const auto& Item = Value.GetArrayItems()[Index];
                const FString Field = TEXT("sockets/") + FString::FromInt(Index);
                if (!TryRequireObjectFields(*Item, {TEXT("name"), TEXT("location"), TEXT("rotation"), TEXT("scale")}, Field, OutError)) return false;
                FString Name;
                FSocketValue Socket;
                if (!TryReadString(*FindObjectField(*Item, TEXT("name")), Name, Field + TEXT("/name"), OutError)) return false;
                if (Name.TrimStartAndEnd().IsEmpty())
                    return Fail(OutError, ValueFailure(TEXT("asset_data_socket_invalid"),
                        TEXT("The socket name must not be blank."), Field + TEXT("/name"), FindObjectField(*Item, TEXT("name")), TEXT("nonblank socket name")));
                if (!TryReadVector(*FindObjectField(*Item, TEXT("location")), Socket.Location, Field + TEXT("/location"), OutError)
                    || !TryReadRotator(*FindObjectField(*Item, TEXT("rotation")), Socket.Rotation, Field + TEXT("/rotation"), OutError)
                    || !TryReadVector(*FindObjectField(*Item, TEXT("scale")), Socket.Scale, Field + TEXT("/scale"), OutError)) return false;
                if (Socket.Scale.GetAbsMin() <= 0.0)
                {
                    const TCHAR* Axis = FMath::Abs(Socket.Scale.X) <= 0.0 ? TEXT("x") : FMath::Abs(Socket.Scale.Y) <= 0.0 ? TEXT("y") : TEXT("z");
                    return Fail(OutError, ValueFailure(TEXT("asset_data_socket_invalid"),
                        TEXT("The socket scale component must be nonzero."), Field + TEXT("/scale/") + Axis,
                        FindObjectField(*FindObjectField(*Item, TEXT("scale")), Axis), TEXT("nonzero finite number")));
                }
                Socket.Name = FName(*Name);
                if (Seen.Contains(Socket.Name))
                    return Fail(OutError, JsonFailure(TEXT("asset_data_socket_duplicate"),
                        TEXT("The socket name is duplicated."), Field + TEXT("/name"),
                        MakeShared<FJsonValueString>(Name), MakeShared<FJsonValueString>(TEXT("unique socket name"))));
                Seen.Add(Socket.Name);
                Out.Add(MoveTemp(Socket));
            }
            return true;
        }

		bool TryApplySocketPresence(
			const FStructuredAssetChangeView& Change,
			const TArray<FSocketValue>& Current,
			TArray<FSocketValue>& Out,
			FExecutionMutationError& OutError)
		{
			if (Change.Presence == TEXT("clear"))
			{
				Out.Reset();
				return true;
			}
            if (!Change.CanonicalValue)
                return Fail(OutError, ValueFailure(TEXT("asset_data_sockets_invalid"),
                    TEXT("The socket operation requires a value."), TEXT("sockets"), nullptr, TEXT("socket operation value")));
			if (Change.Presence == TEXT("replace"))
			{
				return TryReadSockets(
					*Change.CanonicalValue,
					Out,
					OutError);
			}
			if (Change.Presence == TEXT("set"))
			{
				TArray<FSocketValue> Updates;
				if (!TryReadSockets(
						*Change.CanonicalValue,
						Updates,
						OutError))
				{
					return false;
				}
				Out = Current;
				for (FSocketValue& Update : Updates)
				{
					const int32 Existing =
						Out.IndexOfByPredicate(
							[&Update](const FSocketValue& Item)
							{
								return Item.Name == Update.Name;
							});
					if (Existing == INDEX_NONE)
					{
						Out.Add(MoveTemp(Update));
					}
					else
					{
						Out[Existing] = MoveTemp(Update);
					}
				}
				return true;
			}
			if (Change.Presence == TEXT("remove"))
			{
				TSet<FName> Names;
				if (!TryReadRemovalNames(
						*Change.CanonicalValue,
						Names,
						OutError,
						TEXT("asset_data_socket_remove_invalid"), TEXT("sockets")))
				{
					return false;
				}
				Out = Current;
				Out.RemoveAll(
					[&Names](const FSocketValue& Item)
					{
						return Names.Contains(Item.Name);
					});
				return true;
			}
            return Fail(OutError, JsonFailure(TEXT("asset_data_socket_presence_invalid"),
                TEXT("The socket operation is not supported."), TEXT("sockets/presence"),
                MakeShared<FJsonValueString>(Change.Presence), MakeShared<FJsonValueString>(TEXT("clear, set, replace, or remove"))));
		}

		bool TryReadBuildSettings(
			const FUeAgentCanonicalValue& Value,
			int32 SourceModelCount,
			const TArray<FMeshBuildSettings>& Current,
			TArray<FMeshBuildSettings>& Out,
			FExecutionMutationError& OutError)
		{
            if (!TryRequireObjectFields(Value, {TEXT("lods")}, TEXT("build_settings"), OutError)) return false;
            const FUeAgentCanonicalValue* Lods = FindObjectField(Value, TEXT("lods"));
            if (!Lods || Lods->GetKind() != EUeAgentValueKind::Array)
                return Fail(OutError, ValueFailure(TEXT("asset_data_build_settings_invalid"),
                    TEXT("LOD build settings must be an array."), TEXT("build_settings/lods"), Lods, TEXT("array")));
			Out = Current;
			TSet<int32> Seen;
            for (int32 EntryIndex = 0; EntryIndex < Lods->GetArrayItems().Num(); ++EntryIndex)
            {
                const auto& Item = Lods->GetArrayItems()[EntryIndex];
                const FString Field = TEXT("build_settings/lods/") + FString::FromInt(EntryIndex);
                if (!TryRequireObjectFields(*Item, {TEXT("lod_index"), TEXT("recompute_normals"), TEXT("recompute_tangents"),
                    TEXT("compute_weighted_normals"), TEXT("remove_degenerates"), TEXT("use_mikk_tspace"), TEXT("generate_lightmap_uvs")}, Field, OutError)) return false;
				int32 Index = INDEX_NONE;
				bool bRecomputeNormals = false;
				bool bRecomputeTangents = false;
				bool bComputeWeightedNormals = false;
				bool bRemoveDegenerates = false;
				bool bUseMikk = false;
				bool bGenerateLightmapUvs = false;
                if (!TryReadInt32(*FindObjectField(*Item, TEXT("lod_index")), Index, Field + TEXT("/lod_index"), OutError)) return false;
                if (Index < 0 || Index >= SourceModelCount)
                    return Fail(OutError, JsonFailure(TEXT("asset_data_build_settings_lod_invalid"),
                        TEXT("The LOD index is outside the mesh source model range."), Field + TEXT("/lod_index"),
                        MakeShared<FJsonValueNumber>(Index), MakeShared<FJsonValueString>(FString::Printf(TEXT("0 <= lod_index < %d"), SourceModelCount))));
                if (Seen.Contains(Index))
                    return Fail(OutError, JsonFailure(TEXT("asset_data_build_settings_lod_invalid"),
                        TEXT("The LOD index is duplicated."), Field + TEXT("/lod_index"),
                        MakeShared<FJsonValueNumber>(Index), MakeShared<FJsonValueString>(TEXT("unique LOD index"))));
                if (!TryReadBool(*FindObjectField(*Item, TEXT("recompute_normals")), bRecomputeNormals, Field + TEXT("/recompute_normals"), OutError)
                    || !TryReadBool(*FindObjectField(*Item, TEXT("recompute_tangents")), bRecomputeTangents, Field + TEXT("/recompute_tangents"), OutError)
                    || !TryReadBool(*FindObjectField(*Item, TEXT("compute_weighted_normals")), bComputeWeightedNormals, Field + TEXT("/compute_weighted_normals"), OutError)
                    || !TryReadBool(*FindObjectField(*Item, TEXT("remove_degenerates")), bRemoveDegenerates, Field + TEXT("/remove_degenerates"), OutError)
                    || !TryReadBool(*FindObjectField(*Item, TEXT("use_mikk_tspace")), bUseMikk, Field + TEXT("/use_mikk_tspace"), OutError)
                    || !TryReadBool(*FindObjectField(*Item, TEXT("generate_lightmap_uvs")), bGenerateLightmapUvs, Field + TEXT("/generate_lightmap_uvs"), OutError)) return false;
				FMeshBuildSettings& Settings = Out[Index];
				Settings.bRecomputeNormals = bRecomputeNormals;
				Settings.bRecomputeTangents = bRecomputeTangents;
				Settings.bComputeWeightedNormals =
					bComputeWeightedNormals;
				Settings.bRemoveDegenerates = bRemoveDegenerates;
				Settings.bUseMikkTSpace = bUseMikk;
				Settings.bGenerateLightmapUVs =
					bGenerateLightmapUvs;
				Seen.Add(Index);
			}
            if (Seen.IsEmpty())
                return Fail(OutError, JsonFailure(TEXT("asset_data_build_settings_invalid"),
                    TEXT("At least one LOD build settings entry is required."), TEXT("build_settings/lods"),
                    MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("one or more entries"))));
            return true;
		}

		struct FOwnedStructBuffer final
		{
			explicit FOwnedStructBuffer(
				UScriptStruct* InType)
				: Type(InType)
			{
				if (Type)
				{
					Data = static_cast<uint8*>(
						FMemory::Malloc(
							Type->GetStructureSize(),
							Type->GetMinAlignment()));
					Type->InitializeStruct(Data);
				}
			}

			~FOwnedStructBuffer()
			{
				if (Type && Data)
				{
					Type->DestroyStruct(Data);
					FMemory::Free(Data);
				}
			}

			FOwnedStructBuffer(const FOwnedStructBuffer&) = delete;
			FOwnedStructBuffer& operator=(
				const FOwnedStructBuffer&) = delete;

			void CopyFrom(const void* Source)
			{
				check(Type && Data && Source);
				Type->CopyScriptStruct(Data, Source);
			}

			UScriptStruct* Type = nullptr;
			uint8* Data = nullptr;
		};

		struct FTableRowValue final
		{
			FName Name;
			TSharedPtr<FOwnedStructBuffer, ESPMode::ThreadSafe>
				Data;
		};

		bool TryWriteCanonicalProperty(
			IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
			const FProperty& Property,
			void* ValuePtr,
			const FUeAgentCanonicalValue& Value,
			FExecutionMutationError& OutError, const FString& Field);

		bool TryWriteCanonicalStruct(
			IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
			UStruct& Struct,
			void* StructData,
			const FUeAgentCanonicalValue& Value,
			FExecutionMutationError& OutError, const FString& Field)
		{
            if (Value.GetKind() != EUeAgentValueKind::Object)
                return Fail(OutError, ValueFailure(TEXT("asset_data_data_table_struct_invalid"),
                    TEXT("A structure value must be an object."), Field, &Value, TEXT("object for ") + Struct.GetPathName()));
			TSet<const FProperty*> Written;
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Item
				: Value.GetObjectItems())
			{
				FProperty* Property =
					FindFProperty<FProperty>(
						&Struct,
						FName(*Item.Key));
                const FString Path = ChildPath(Field, Item.Key);
                if (!Property || Written.Contains(Property) || Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
                    return Fail(OutError, JsonFailure(TEXT("asset_data_data_table_field_invalid"),
                        !Property ? TEXT("The field does not exist in the row structure.")
                            : Written.Contains(Property) ? TEXT("The row structure field is specified more than once.")
                                : TEXT("Transient or deprecated fields cannot be authored."),
                        Path, MakeShared<FJsonValueString>(Item.Key),
                        MakeShared<FJsonValueString>(TEXT("unique writable field of ") + Struct.GetPathName())));
				void* PropertyValue =
					Property->ContainerPtrToValuePtr<void>(
						StructData);
				if (!TryWriteCanonicalProperty(
						Resolver,
						*Property,
						PropertyValue,
						Item.Value.Get(),
						OutError, Path))
                {
                    if (OutError.PropertyPath.IsEmpty()) OutError.PropertyPath = Field;
                    return false;
                }
				Written.Add(Property);
			}
			return true;
		}

		bool TryWriteCanonicalProperty(
			IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
			const FProperty& Property,
			void* ValuePtr,
			const FUeAgentCanonicalValue& Value,
			FExecutionMutationError& OutError, const FString& Field)
		{
			if (const FBoolProperty* Bool =
				CastField<FBoolProperty>(&Property))
			{
				bool Parsed = false;
                if (!TryReadBool(Value, Parsed, Field, OutError)) return false;
				Bool->SetPropertyValue(ValuePtr, Parsed);
				return true;
			}
			if (const FByteProperty* Byte =
				CastField<FByteProperty>(&Property);
				Byte && Byte->Enum)
			{
				int64 EnumValue = 0;
				bool bParsed = false;
				if (Value.GetKind() == EUeAgentValueKind::String)
				{
					bParsed = UeAgentAuthoringTypeDescriptors::TryResolveEnumValueName(Byte->Enum, Value.GetText(), EnumValue);
				}
				else
				{
                    int32 Parsed = INDEX_NONE;
                    if (!TryReadInt32(Value, Parsed, Field, OutError)) return false;
                    EnumValue = Parsed;
                    bParsed = true;
				}
				if (!bParsed || EnumValue < 0 || EnumValue > MAX_uint8
					|| !Byte->Enum->IsValidEnumValue(EnumValue))
				{
                    return Fail(OutError, ValueFailure(TEXT("asset_data_data_table_value_invalid"),
                        TEXT("The value is not a valid member of the byte enum."), Field, &Value,
                        TEXT("uint8 member of ") + Byte->Enum->GetPathName()));
				}
				Byte->SetPropertyValue(
					ValuePtr,
					static_cast<uint8>(EnumValue));
				return true;
			}
			if (const FNumericProperty* Numeric =
				CastField<FNumericProperty>(&Property))
			{
				if (Numeric->IsInteger())
				{
					if (Value.GetKind() != EUeAgentValueKind::Integer
						&& Value.GetKind() != EUeAgentValueKind::Decimal)
					{
                    return Fail(OutError, ValueFailure(TEXT("asset_data_data_table_value_invalid"),
                        TEXT("An integer property requires a numeric value."), Field, &Value, Property.GetCPPType()));
					}
					const FString& Lexeme = Value.GetLexeme();
					if (Lexeme.StartsWith(TEXT("-")))
					{
						int64 Parsed = 0;
						if (!TryParseInt64Number(Lexeme, Parsed)
							|| !Numeric->CanHoldValue(Parsed))
						{
                        return Fail(OutError, ValueFailure(TEXT("asset_data_data_table_value_invalid"),
                            TEXT("The number is not an integer representable by the target property."), Field, &Value, Property.GetCPPType()));
						}
						Numeric->SetIntPropertyValue(ValuePtr, Parsed);
					}
					else
					{
						uint64 Parsed = 0;
						if (!TryParseUInt64Number(Lexeme, Parsed)
							|| !Numeric->CanHoldValue(Parsed))
						{
                        return Fail(OutError, ValueFailure(TEXT("asset_data_data_table_value_invalid"),
                            TEXT("The number is not an integer representable by the target property."), Field, &Value, Property.GetCPPType()));
						}
						Numeric->SetIntPropertyValue(ValuePtr, Parsed);
					}
				}
				else
				{
					double Parsed = 0.0;
                    if (!TryReadNumber(Value, Parsed, Field, OutError)) return false;
                    if (!Numeric->CanHoldValue(Parsed))
                        return Fail(OutError, ValueFailure(TEXT("asset_data_data_table_value_invalid"),
                            TEXT("The number cannot be represented by the target property."), Field, &Value, Property.GetCPPType()));
					Numeric->SetFloatingPointPropertyValue(
						ValuePtr,
						Parsed);
				}
				return true;
			}
			if (const FStrProperty* String =
				CastField<FStrProperty>(&Property))
			{
				FString Parsed;
                if (!TryReadString(Value, Parsed, Field, OutError)) return false;
				String->SetPropertyValue(ValuePtr, Parsed);
				return true;
			}
			if (const FNameProperty* Name =
				CastField<FNameProperty>(&Property))
			{
				FString Parsed;
                if (!TryReadString(Value, Parsed, Field, OutError)) return false;
				Name->SetPropertyValue(ValuePtr, FName(*Parsed));
				return true;
			}
			if (const FTextProperty* Text =
				CastField<FTextProperty>(&Property))
			{
				FString Parsed;
                if (!TryReadString(Value, Parsed, Field, OutError)) return false;
				Text->SetPropertyValue(
					ValuePtr,
					FText::FromString(Parsed));
				return true;
			}
			if (const FEnumProperty* Enum =
				CastField<FEnumProperty>(&Property))
			{
				int64 EnumValue = 0;
				bool bParsed = false;
				if (Value.GetKind() == EUeAgentValueKind::String)
				{
					bParsed = UeAgentAuthoringTypeDescriptors::TryResolveEnumValueName(Enum->GetEnum(), Value.GetText(), EnumValue);
				}
				else
				{
                    int32 Parsed = INDEX_NONE;
                    if (!TryReadInt32(Value, Parsed, Field, OutError)) return false;
                    EnumValue = Parsed;
                    bParsed = true;
				}
				if (!bParsed
					|| !Enum->GetEnum()->IsValidEnumValue(
						EnumValue))
				{
                    return Fail(OutError, ValueFailure(TEXT("asset_data_data_table_value_invalid"),
                        TEXT("The value is not a valid member of the enum."), Field, &Value,
                        TEXT("member of ") + Enum->GetEnum()->GetPathName()));
				}
				Enum->GetUnderlyingProperty()
					->SetIntPropertyValue(ValuePtr, EnumValue);
				return true;
			}
			if (const FSoftObjectProperty* Soft =
				CastField<FSoftObjectProperty>(&Property))
			{
				if (Value.GetKind() == EUeAgentValueKind::Null)
				{
					Soft->SetPropertyValue(
						ValuePtr,
						FSoftObjectPtr());
					return true;
				}
				FString Path;
                if (!TryReadString(Value, Path, Field, OutError)) return false;
				Soft->SetPropertyValue(
					ValuePtr,
					FSoftObjectPtr(FSoftObjectPath(Path)));
				return true;
			}
			if (const FObjectPropertyBase* Object =
				CastField<FObjectPropertyBase>(&Property))
			{
				if (Value.GetKind() == EUeAgentValueKind::Null)
				{
					Object->SetObjectPropertyValue(ValuePtr, nullptr);
					return true;
				}
				FString Ref;
                if (!TryReadString(Value, Ref, Field, OutError)) return false;
				UObject* Resolved = nullptr;
				if (!Resolver.TryResolveExisting(
						Ref,
						Resolved,
						OutError))
				{
					return false;
				}
				if (!Resolved
					|| !Resolved->IsA(Object->PropertyClass))
				{
                    auto Actual = MakeShared<FJsonObject>();
                    Actual->SetStringField(TEXT("reference"), Ref);
                    Actual->SetField(TEXT("resolved_class"), ObjectClassValue(Resolved));
                    return Fail(OutError, JsonFailure(TEXT("asset_data_data_table_reference_type_invalid"),
                        TEXT("The resolved object does not satisfy the property class."), Field,
                        MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(Object->PropertyClass->GetPathName())));
				}
				Object->SetObjectPropertyValue(
					ValuePtr,
					Resolved);
				return true;
			}
			if (const FStructProperty* Struct =
				CastField<FStructProperty>(&Property))
			{
				return TryWriteCanonicalStruct(
					Resolver,
					*Struct->Struct,
					ValuePtr,
					Value,
					OutError, Field);
			}
			if (const FArrayProperty* Array =
				CastField<FArrayProperty>(&Property))
			{
				if (Value.GetKind() != EUeAgentValueKind::Array)
				{
                    return Fail(OutError, ValueFailure(TEXT("asset_data_data_table_value_invalid"),
                        TEXT("The container property requires an array."), Field, &Value, TEXT("array for ") + Property.GetCPPType()));
				}
				FScriptArrayHelper Helper(Array, ValuePtr);
				Helper.EmptyValues();
                int32 InputIndex = 0;
                for (const FUeAgentCanonicalValueRef& Item : Value.GetArrayItems())
                {
                    const FString ItemPath = Field + TEXT("/") + FString::FromInt(InputIndex++);
					const int32 Index = Helper.AddValue();
					if (!TryWriteCanonicalProperty(
							Resolver,
							*Array->Inner,
							Helper.GetRawPtr(Index),
							Item.Get(),
							OutError, Field + TEXT("/") + FString::FromInt(Index)))
					{
						return false;
					}
				}
				return true;
			}
			if (const FSetProperty* Set =
				CastField<FSetProperty>(&Property))
			{
				if (Value.GetKind() != EUeAgentValueKind::Array)
				{
                    return Fail(OutError, ValueFailure(TEXT("asset_data_data_table_value_invalid"),
                        TEXT("The container property requires an array."), Field, &Value, TEXT("array for ") + Property.GetCPPType()));
				}
				FScriptSetHelper Helper(Set, ValuePtr);
				Helper.EmptyElements();
                int32 InputIndex = 0;
				for (const FUeAgentCanonicalValueRef& Item
					: Value.GetArrayItems())
				{
                    const FString ItemPath = Field + TEXT("/") + FString::FromInt(InputIndex++);
					const int32 Index =
						Helper.AddDefaultValue_Invalid_NeedsRehash();
					if (!TryWriteCanonicalProperty(
							Resolver,
							*Set->ElementProp,
							Helper.GetElementPtr(Index),
							Item.Get(),
							OutError, ItemPath))
					{
						return false;
					}
				}
				Helper.Rehash();
				return true;
			}
			if (const FMapProperty* Map =
				CastField<FMapProperty>(&Property))
			{
				if (Value.GetKind() != EUeAgentValueKind::Array)
				{
                    return Fail(OutError, ValueFailure(TEXT("asset_data_data_table_value_invalid"),
                        TEXT("The container property requires an array."), Field, &Value, TEXT("array for ") + Property.GetCPPType()));
				}
				FScriptMapHelper Helper(Map, ValuePtr);
				Helper.EmptyValues();
                int32 InputIndex = 0;
                for (const FUeAgentCanonicalValueRef& Item : Value.GetArrayItems())
                {
                    const FString ItemPath = Field + TEXT("/") + FString::FromInt(InputIndex++);
                    if (!TryRequireObjectFields(*Item, {TEXT("key"), TEXT("value")}, ItemPath, OutError)) return false;
					const int32 Index =
						Helper.AddDefaultValue_Invalid_NeedsRehash();
					if (!TryWriteCanonicalProperty(
							Resolver,
							*Map->KeyProp,
							Helper.GetKeyPtr(Index),
							*FindObjectField(*Item, TEXT("key")),
							OutError, ItemPath + TEXT("/key"))
						|| !TryWriteCanonicalProperty(
							Resolver,
							*Map->ValueProp,
							Helper.GetValuePtr(Index),
							*FindObjectField(*Item, TEXT("value")),
							OutError, ItemPath + TEXT("/value")))
					{
						return false;
					}
				}
				Helper.Rehash();
				return true;
			}
            return Fail(OutError, ValueFailure(TEXT("asset_data_data_table_property_type_unsupported"),
                TEXT("The property type is not supported for DataTable authoring: ") + Property.GetCPPType(),
                Field, &Value, TEXT("boolean, enum, numeric, string, name, text, object reference, structure, array, set, or map")));
		}

		TArray<FTableRowValue> CaptureRows(
			UDataTable& Table,
			UScriptStruct* RowStruct)
		{
			TArray<FTableRowValue> Out;
			if (!RowStruct)
			{
				return Out;
			}
			for (const TPair<FName, uint8*>& Row
				: Table.GetRowMap())
			{
				FTableRowValue Value;
				Value.Name = Row.Key;
				Value.Data =
					MakeShared<
						FOwnedStructBuffer,
						ESPMode::ThreadSafe>(RowStruct);
				Value.Data->CopyFrom(Row.Value);
				Out.Add(MoveTemp(Value));
			}
			return Out;
		}

		bool TryReadRows(
			IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
			UScriptStruct* RowStruct,
			const FUeAgentCanonicalValue& Value,
			TArray<FTableRowValue>& Out,
			FExecutionMutationError& OutError)
		{
            if (!RowStruct || !RowStruct->IsChildOf(FTableRowBase::StaticStruct()))
                return Fail(OutError, NativeFailure(TEXT("asset_data_data_table_rows_invalid"),
                    TEXT("The resolved row structure must derive from TableRowBase."), TEXT("row_struct_ref"),
                    RowStruct ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(RowStruct->GetPathName())) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()),
                    MakeShared<FJsonValueString>(FTableRowBase::StaticStruct()->GetPathName())));
            if (Value.GetKind() != EUeAgentValueKind::Array)
                return Fail(OutError, ValueFailure(TEXT("asset_data_data_table_rows_invalid"),
                    TEXT("DataTable rows must be an array."), TEXT("rows"), &Value, TEXT("array")));
			Out.Reset();
			TSet<FName> Names;
            for (int32 Index = 0; Index < Value.GetArrayItems().Num(); ++Index)
            {
                const auto& Item = Value.GetArrayItems()[Index];
                const FString Field = TEXT("rows/") + FString::FromInt(Index);
                if (!TryRequireObjectFields(*Item, {TEXT("name"), TEXT("values")}, Field, OutError)) return false;
                FString Name;
                if (!TryReadString(*FindObjectField(*Item, TEXT("name")), Name, Field + TEXT("/name"), OutError)) return false;
                if (Name.TrimStartAndEnd().IsEmpty())
                    return Fail(OutError, ValueFailure(TEXT("asset_data_data_table_row_invalid"),
                        TEXT("A DataTable row name must not be blank."), Field + TEXT("/name"), FindObjectField(*Item, TEXT("name")), TEXT("nonblank row name")));
				FTableRowValue Row;
				Row.Name = FName(*Name);
				if (Names.Contains(Row.Name))
				{
                    return Fail(OutError, JsonFailure(TEXT("asset_data_data_table_row_duplicate"),
                        TEXT("The DataTable row name is duplicated."), Field + TEXT("/name"),
                        MakeShared<FJsonValueString>(Name), MakeShared<FJsonValueString>(TEXT("unique row name"))));
				}
				Row.Data =
					MakeShared<
						FOwnedStructBuffer,
						ESPMode::ThreadSafe>(RowStruct);
				if (!TryWriteCanonicalStruct(
						Resolver,
						*RowStruct,
						Row.Data->Data,
						*FindObjectField(*Item, TEXT("values")),
						OutError, Field + TEXT("/values")))
				{
					return false;
				}
				Names.Add(Row.Name);
				Out.Add(MoveTemp(Row));
			}
			return true;
		}

		bool WriteRows(
			UDataTable& Table,
			UScriptStruct* RowStruct,
			const TArray<FTableRowValue>& Rows,
            FExecutionMutationError& OutError)
		{
			if (!RowStruct)
			{
                OutError = NativeFailure(TEXT("asset_data_data_table_row_struct_missing"),
                    TEXT("Prepared DataTable rows have no row structure."), TEXT("row_struct"),
                    MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("available row structure")));
                OutError.TargetPath = Table.GetPathName();
                return false;
			}
			for (const FTableRowValue& Row : Rows)
			{
				if (!Row.Data.IsValid()
					|| Row.Data->Type != RowStruct
					|| !Row.Data->Data)
				{
                    const FString Field = TEXT("rows/") + Row.Name.ToString().Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
                    const bool bTypeMismatch = Row.Data.IsValid() && Row.Data->Type != RowStruct;
                    OutError = NativeFailure(TEXT("asset_data_data_table_row_storage_invalid"),
                        !Row.Data.IsValid() ? TEXT("The prepared DataTable row has no captured value.")
                            : bTypeMismatch ? TEXT("The prepared row structure does not match the table row structure.")
                                : TEXT("The prepared DataTable row has no value storage."),
                        Field, bTypeMismatch && Row.Data->Type
                            ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Row.Data->Type->GetPathName()))
                            : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()),
                        MakeShared<FJsonValueString>(RowStruct->GetPathName()));
                    OutError.TargetPath = Table.GetPathName();
                    return false;
				}
			}
			TMap<FName, const uint8*> RowViews;
			for (const FTableRowValue& Row : Rows)
			{
				RowViews.Add(Row.Name, Row.Data->Data);
			}
			return TryReplaceDataTableRows(Table, RowStruct, RowViews, OutError);
		}

        bool TryReadBounds(const FUeAgentCanonicalValue& Value, FBoxSphereBounds& Out, FExecutionMutationError& OutError)
        {
            if (!TryRequireObjectFields(Value, {TEXT("min"), TEXT("max")}, TEXT("bounds"), OutError)) return false;
            FVector Min, Max;
            if (!TryReadVector(*FindObjectField(Value, TEXT("min")), Min, TEXT("bounds/min"), OutError)
                || !TryReadVector(*FindObjectField(Value, TEXT("max")), Max, TEXT("bounds/max"), OutError)) return false;
            if (Min.X > Max.X || Min.Y > Max.Y || Min.Z > Max.Z)
            {
                const TCHAR* Axis = Min.X > Max.X ? TEXT("x") : Min.Y > Max.Y ? TEXT("y") : TEXT("z");
                const double Minimum = Min.X > Max.X ? Min.X : Min.Y > Max.Y ? Min.Y : Min.Z;
                const double Maximum = Min.X > Max.X ? Max.X : Min.Y > Max.Y ? Max.Y : Max.Z;
                return Fail(OutError, JsonFailure(TEXT("asset_data_bounds_invalid"),
                    TEXT("The minimum bound component exceeds the maximum."), FString(TEXT("bounds/min/")) + Axis,
                    MakeShared<FJsonValueNumber>(Minimum), MakeShared<FJsonValueNumber>(Maximum)));
            }
            Out = FBoxSphereBounds(FBox(Min, Max));
            return true;
        }

		bool TryBuildProjectionString(
			const FString& Value,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& Out,
			FExecutionMutationError& OutError)
		{
			FUeAgentStateSemanticError Error;
			return FUeAgentCanonicalValue::TryString(
					Value,
					Resources,
					Out,
					Error)
				? true
				: Fail(OutError, MoveTemp(Error));
		}

		bool TryBuildProjectionNull(
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& Out,
			FExecutionMutationError& OutError)
		{
			FUeAgentStateSemanticError Error;
			return FUeAgentCanonicalValue::TryNull(
					Resources,
					Out,
					Error)
				? true
				: Fail(OutError, MoveTemp(Error));
		}

		bool TryBuildProjectionBoolean(
			const bool Value,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& Out,
			FExecutionMutationError& OutError)
		{
			FUeAgentStateSemanticError Error;
			return FUeAgentCanonicalValue::TryBoolean(
					Value,
					Resources,
					Out,
					Error)
				? true
				: Fail(OutError, MoveTemp(Error));
		}

		bool TryBuildProjectionInteger(
			const FString& Lexeme,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& Out,
			FExecutionMutationError& OutError)
		{
			FUeAgentStateSemanticError Error;
			return FUeAgentCanonicalValue::TryInteger(
					Lexeme,
					Resources,
					Out,
					Error)
				? true
				: Fail(OutError, MoveTemp(Error));
		}

        bool TryBuildProjectionDecimal(const double Value, FUeAgentResourceTracker& Resources,
            FUeAgentCanonicalValuePtr& Out, FExecutionMutationError& OutError, const FString& Field)
        {
            if (!FMath::IsFinite(Value))
                return Fail(OutError, NativeFailure(TEXT("asset_data_structured_asset_projection_decimal_invalid"),
                    TEXT("The native numeric value is not finite."), Field,
                    MakeShared<FJsonValueString>(FString::SanitizeFloat(Value)), MakeShared<FJsonValueString>(TEXT("finite number"))));
            if (FUeAgentCanonicalValue::TryDecimal(FString::SanitizeFloat(Value), Resources, Out, OutError)) return true;
            // This call constructs exactly this native field; the codec's root is local to the call.
            OutError.PropertyPath = Field;
            OutError.SourceKind = TEXT("native_result");
            return false;
        }

		bool TryBuildProjectionArray(
			TArray<FUeAgentCanonicalValueRef> Items,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& Out,
			FExecutionMutationError& OutError)
		{
			FUeAgentStateSemanticError Error;
			return FUeAgentCanonicalValue::TryArray(
					Items,
					Resources,
					Out,
					Error)
				? true
				: Fail(OutError, MoveTemp(Error));
		}

		bool TryBuildProjectionObject(
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Items,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& Out,
			FExecutionMutationError& OutError)
		{
			FUeAgentStateSemanticError Error;
			return FUeAgentCanonicalValue::TryObject(
					Items,
					Resources,
					Out,
					Error)
				? true
				: Fail(OutError, MoveTemp(Error));
		}

		FString ProjectionFingerprint(
			const FUeAgentCanonicalValue& Value)
		{
			switch (Value.GetKind())
			{
			case EUeAgentValueKind::Null:
				return TEXT("n");
			case EUeAgentValueKind::Boolean:
				return Value.GetBoolean() ? TEXT("b1") : TEXT("b0");
			case EUeAgentValueKind::String:
				return FString::Printf(
					TEXT("s%d:%s"),
					Value.GetText().Len(),
					*Value.GetText());
			case EUeAgentValueKind::Integer:
				return TEXT("i") + Value.GetLexeme();
			case EUeAgentValueKind::Decimal:
				return TEXT("d") + Value.GetLexeme();
			case EUeAgentValueKind::Array:
				{
					FString Out = TEXT("a[");
					for (const FUeAgentCanonicalValueRef& Item
						: Value.GetArrayItems())
					{
						Out += ProjectionFingerprint(Item.Get()) + TEXT(";");
					}
					return Out + TEXT("]");
				}
			case EUeAgentValueKind::Object:
				{
					FString Out = TEXT("o{");
					for (const TPair<FString, FUeAgentCanonicalValueRef>& Item
						: Value.GetObjectItems())
					{
						Out += FString::Printf(
							TEXT("%d:%s="),
							Item.Key.Len(),
							*Item.Key);
						Out += ProjectionFingerprint(Item.Value.Get()) + TEXT(";");
					}
					return Out + TEXT("}");
				}
			default:
				return TEXT("unsupported");
			}
		}

		bool IsUnsignedProjectionNumber(
			const FNumericProperty& Property)
		{
			return CastField<FByteProperty>(&Property)
				|| CastField<FUInt16Property>(&Property)
				|| CastField<FUInt32Property>(&Property)
				|| CastField<FUInt64Property>(&Property);
		}

		bool TryProjectCanonicalProperty(
			const FProperty& Property,
			const void* ValuePtr,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& Out,
			FExecutionMutationError& OutError, const FString& Field);

		bool TryProjectCanonicalStruct(
			UStruct& Struct,
			const void* StructData,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& Out,
			FExecutionMutationError& OutError, const FString& Field)
		{
            ON_SCOPE_EXIT
            {
                if (!OutError.Code.IsEmpty())
                {
                    OutError.SourceKind = TEXT("native_result");
                    OutError.Source = {};
                    OutError.SemanticPointer.Reset();
                    if (OutError.PropertyPath.IsEmpty()) OutError.PropertyPath = Field;
                }
            };
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Items;
			for (TFieldIterator<FProperty> It(&Struct); It; ++It)
			{
				FProperty* Property = *It;
				if (!Property
					|| Property->HasAnyPropertyFlags(
						CPF_Transient | CPF_Deprecated))
				{
					continue;
				}
				FUeAgentCanonicalValuePtr Value;
				if (!TryProjectCanonicalProperty(
						*Property,
						Property->ContainerPtrToValuePtr<void>(StructData),
						Resources,
						Value,
						OutError, ChildPath(Field, Property->GetName())))
				{
					return false;
				}
				Items.Emplace(Property->GetName(), Value.ToSharedRef());
			}
			return TryBuildProjectionObject(
				MoveTemp(Items),
				Resources,
				Out,
				OutError);
		}

		bool TryProjectCanonicalProperty(
			const FProperty& Property,
			const void* ValuePtr,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& Out,
			FExecutionMutationError& OutError, const FString& Field)
		{
            bool bPrimitiveValue = true;
            ON_SCOPE_EXIT
            {
                if (!OutError.Code.IsEmpty())
                {
                    OutError.SourceKind = TEXT("native_result");
                    OutError.Source = {};
                    OutError.SemanticPointer.Reset();
                    if (bPrimitiveValue || OutError.PropertyPath.IsEmpty()) OutError.PropertyPath = Field;
                }
            };
			if (const FBoolProperty* Bool =
				CastField<FBoolProperty>(&Property))
			{
				return TryBuildProjectionBoolean(
					Bool->GetPropertyValue(ValuePtr),
					Resources,
					Out,
					OutError);
			}
			if (const FByteProperty* Byte =
				CastField<FByteProperty>(&Property);
				Byte && Byte->Enum)
			{
				return TryBuildProjectionString(
					Byte->Enum->GetNameStringByValue(
						Byte->GetPropertyValue(ValuePtr)),
					Resources,
					Out,
					OutError);
			}
			if (const FEnumProperty* Enum =
				CastField<FEnumProperty>(&Property))
			{
				const int64 Raw = Enum->GetUnderlyingProperty()
					->GetSignedIntPropertyValue(ValuePtr);
				return TryBuildProjectionString(
					Enum->GetEnum()->GetNameStringByValue(Raw),
					Resources,
					Out,
					OutError);
			}
			if (const FNumericProperty* Numeric =
				CastField<FNumericProperty>(&Property))
			{
				if (Numeric->IsFloatingPoint())
				{
					return TryBuildProjectionDecimal(
						Numeric->GetFloatingPointPropertyValue(ValuePtr),
						Resources,
						Out,
						OutError, Field);
				}
				return TryBuildProjectionInteger(
					IsUnsignedProjectionNumber(*Numeric)
						? FString::Printf(
							TEXT("%llu"),
							static_cast<unsigned long long>(
								Numeric->GetUnsignedIntPropertyValue(ValuePtr)))
						: FString::Printf(
							TEXT("%lld"),
							static_cast<long long>(
								Numeric->GetSignedIntPropertyValue(ValuePtr))),
					Resources,
					Out,
					OutError);
			}
			if (const FStrProperty* String =
				CastField<FStrProperty>(&Property))
			{
				return TryBuildProjectionString(
					String->GetPropertyValue(ValuePtr),
					Resources,
					Out,
					OutError);
			}
			if (const FNameProperty* Name =
				CastField<FNameProperty>(&Property))
			{
				return TryBuildProjectionString(
					Name->GetPropertyValue(ValuePtr).ToString(),
					Resources,
					Out,
					OutError);
			}
			if (const FTextProperty* Text =
				CastField<FTextProperty>(&Property))
			{
				return TryBuildProjectionString(
					Text->GetPropertyValue(ValuePtr).ToString(),
					Resources,
					Out,
					OutError);
			}
			if (const FSoftObjectProperty* Soft =
				CastField<FSoftObjectProperty>(&Property))
			{
				const FSoftObjectPtr Value = Soft->GetPropertyValue(ValuePtr);
				return Value.IsNull()
					? TryBuildProjectionNull(Resources, Out, OutError)
					: TryBuildProjectionString(
						Value.ToSoftObjectPath().ToString(),
						Resources,
						Out,
						OutError);
			}
			if (const FObjectPropertyBase* Object =
				CastField<FObjectPropertyBase>(&Property))
			{
				const UObject* Value = Object->GetObjectPropertyValue(ValuePtr);
				return Value
					? TryBuildProjectionString(
						Value->GetPathName(),
						Resources,
						Out,
						OutError)
					: TryBuildProjectionNull(Resources, Out, OutError);
			}
			if (const FStructProperty* Struct =
				CastField<FStructProperty>(&Property))
			{
                bPrimitiveValue = false;
				return TryProjectCanonicalStruct(
					*Struct->Struct,
					ValuePtr,
					Resources,
					Out,
					OutError, Field);
			}
			if (const FArrayProperty* Array =
				CastField<FArrayProperty>(&Property))
			{
                bPrimitiveValue = false;
				FScriptArrayHelper Helper(
					Array,
					const_cast<void*>(ValuePtr));
				TArray<FUeAgentCanonicalValueRef> Items;
				Items.Reserve(Helper.Num());
				for (int32 Index = 0; Index < Helper.Num(); ++Index)
				{
					FUeAgentCanonicalValuePtr Item;
					if (!TryProjectCanonicalProperty(
							*Array->Inner,
							Helper.GetRawPtr(Index),
							Resources,
							Item,
							OutError, Field + TEXT("/") + FString::FromInt(Index)))
					{
						return false;
					}
					Items.Add(Item.ToSharedRef());
				}
				return TryBuildProjectionArray(
					MoveTemp(Items),
					Resources,
					Out,
					OutError);
			}
			if (const FSetProperty* Set =
				CastField<FSetProperty>(&Property))
			{
                bPrimitiveValue = false;
				FScriptSetHelper Helper(Set, const_cast<void*>(ValuePtr));
				TArray<FUeAgentCanonicalValueRef> Items;
				for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
				{
					if (!Helper.IsValidIndex(Index))
					{
						continue;
					}
					FUeAgentCanonicalValuePtr Item;
					if (!TryProjectCanonicalProperty(
							*Set->ElementProp,
							Helper.GetElementPtr(Index),
							Resources,
							Item,
							OutError, Field + TEXT("/") + FString::FromInt(Index)))
					{
						return false;
					}
					Items.Add(Item.ToSharedRef());
				}
				Items.Sort(
					[](const FUeAgentCanonicalValueRef& Left,
						const FUeAgentCanonicalValueRef& Right)
					{
						return ProjectionFingerprint(Left.Get())
							< ProjectionFingerprint(Right.Get());
					});
				return TryBuildProjectionArray(
					MoveTemp(Items),
					Resources,
					Out,
					OutError);
			}
			if (const FMapProperty* Map =
				CastField<FMapProperty>(&Property))
			{
                bPrimitiveValue = false;
				FScriptMapHelper Helper(Map, const_cast<void*>(ValuePtr));
				TArray<FUeAgentCanonicalValueRef> Items;
				for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
				{
					if (!Helper.IsValidIndex(Index))
					{
						continue;
					}
					FUeAgentCanonicalValuePtr Key;
					FUeAgentCanonicalValuePtr Value;
					FUeAgentCanonicalValuePtr Entry;
					if (!TryProjectCanonicalProperty(
							*Map->KeyProp,
							Helper.GetKeyPtr(Index),
							Resources,
							Key,
							OutError, Field + TEXT("/") + FString::FromInt(Index) + TEXT("/key"))
						|| !TryProjectCanonicalProperty(
							*Map->ValueProp,
							Helper.GetValuePtr(Index),
							Resources,
							Value,
							OutError, Field + TEXT("/") + FString::FromInt(Index) + TEXT("/value"))
						|| !TryBuildProjectionObject(
							{
								{TEXT("key"), Key.ToSharedRef()},
								{TEXT("value"), Value.ToSharedRef()},
							},
							Resources,
							Entry,
							OutError))
					{
						return false;
					}
					Items.Add(Entry.ToSharedRef());
				}
				Items.Sort(
					[](const FUeAgentCanonicalValueRef& Left,
						const FUeAgentCanonicalValueRef& Right)
					{
						return ProjectionFingerprint(Left.Get())
							< ProjectionFingerprint(Right.Get());
					});
				return TryBuildProjectionArray(
					MoveTemp(Items),
					Resources,
					Out,
					OutError);
			}
            return Fail(OutError, NativeFailure(TEXT("asset_data_data_table_property_type_unsupported"),
                TEXT("The native property type is not supported by DataTable projection."), Field,
                MakeShared<FJsonValueString>(Property.GetCPPType()), MakeShared<FJsonValueString>(TEXT("supported DataTable property type"))));
		}

		bool TryBuildProjectedRows(
			UScriptStruct* RowStruct,
			const TArray<FTableRowValue>& Rows,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& Out,
			FExecutionMutationError& OutError)
		{
            if (!RowStruct)
                return Fail(OutError, NativeFailure(TEXT("asset_data_data_table_row_struct_invalid"),
                    TEXT("The projected DataTable rows have no row structure."), TEXT("row_struct_ref"),
                    MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("available row structure"))));
			TArray<const FTableRowValue*> Ordered;
			Ordered.Reserve(Rows.Num());
			for (const FTableRowValue& Row : Rows)
			{
				Ordered.Add(&Row);
			}
			Ordered.Sort(
				[](const FTableRowValue& Left, const FTableRowValue& Right)
				{
					return Left.Name.LexicalLess(Right.Name);
				});
			TArray<FUeAgentCanonicalValueRef> Items;
			for (const FTableRowValue* Row : Ordered)
			{
				FUeAgentCanonicalValuePtr Name;
				FUeAgentCanonicalValuePtr Values;
				FUeAgentCanonicalValuePtr Item;
                const FString Field = Row ? ChildPath(TEXT("rows"), Row->Name.ToString()) : TEXT("rows");
                if (!Row || !Row->Data.IsValid() || Row->Data->Type != RowStruct)
                    return Fail(OutError, NativeFailure(TEXT("asset_data_data_table_row_storage_invalid"),
                        !Row || !Row->Data.IsValid() ? TEXT("The captured DataTable row has no value storage.")
                            : TEXT("The captured row structure does not match the projected row structure."),
                        Field, Row && Row->Data.IsValid() && Row->Data->Type
                            ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Row->Data->Type->GetPathName()))
                            : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()),
                        MakeShared<FJsonValueString>(RowStruct->GetPathName())));
                if (!TryBuildProjectionString(
						Row->Name.ToString(),
						Resources,
						Name,
						OutError)
					|| !TryProjectCanonicalStruct(
						*RowStruct,
						Row->Data->Data,
						Resources,
						Values,
						OutError, Field + TEXT("/values"))
					|| !TryBuildProjectionObject(
						{
							{TEXT("name"), Name.ToSharedRef()},
							{TEXT("values"), Values.ToSharedRef()},
						},
						Resources,
						Item,
						OutError))
				{
					return false;
				}
				Items.Add(Item.ToSharedRef());
			}
			return TryBuildProjectionArray(
				MoveTemp(Items),
				Resources,
				Out,
				OutError);
		}

		bool TryBuildProjectedVector(
			const FVector& Value,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& Out,
			FExecutionMutationError& OutError, const FString& Field)
		{
			FUeAgentCanonicalValuePtr X;
			FUeAgentCanonicalValuePtr Y;
			FUeAgentCanonicalValuePtr Z;
			return TryBuildProjectionDecimal(Value.X, Resources, X, OutError, Field + TEXT("/x"))
				&& TryBuildProjectionDecimal(Value.Y, Resources, Y, OutError, Field + TEXT("/y"))
				&& TryBuildProjectionDecimal(Value.Z, Resources, Z, OutError, Field + TEXT("/z"))
				&& TryBuildProjectionObject(
					{
						{TEXT("x"), X.ToSharedRef()},
						{TEXT("y"), Y.ToSharedRef()},
						{TEXT("z"), Z.ToSharedRef()},
					},
					Resources,
					Out,
					OutError);
		}

		bool TryBuildProjectedRotator(
			const FRotator& Value,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& Out,
			FExecutionMutationError& OutError, const FString& Field)
		{
			FUeAgentCanonicalValuePtr Pitch;
			FUeAgentCanonicalValuePtr Yaw;
			FUeAgentCanonicalValuePtr Roll;
			return TryBuildProjectionDecimal(
					Value.Pitch,
					Resources,
					Pitch,
					OutError, Field + TEXT("/pitch"))
				&& TryBuildProjectionDecimal(
					Value.Yaw,
					Resources,
					Yaw,
					OutError, Field + TEXT("/yaw"))
				&& TryBuildProjectionDecimal(
					Value.Roll,
					Resources,
					Roll,
					OutError, Field + TEXT("/roll"))
				&& TryBuildProjectionObject(
					{
						{TEXT("pitch"), Pitch.ToSharedRef()},
						{TEXT("yaw"), Yaw.ToSharedRef()},
						{TEXT("roll"), Roll.ToSharedRef()},
					},
					Resources,
					Out,
					OutError);
		}

		struct FProjectedMaterialSlot final
		{
			FName Name;
			FString MaterialPath;
			bool bNullMaterial = true;
		};

		bool TryBuildProjectedMaterials(
			const TArray<FProjectedMaterialSlot>& Slots,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& Out,
			FExecutionMutationError& OutError)
		{
			TArray<FUeAgentCanonicalValueRef> Items;
			for (const FProjectedMaterialSlot& Slot : Slots)
			{
				FUeAgentCanonicalValuePtr Name;
				FUeAgentCanonicalValuePtr Material;
				FUeAgentCanonicalValuePtr Item;
				if (!TryBuildProjectionString(
						Slot.Name.ToString(),
						Resources,
						Name,
						OutError)
					|| !(Slot.bNullMaterial
						? TryBuildProjectionNull(
							Resources,
							Material,
							OutError)
						: TryBuildProjectionString(
							Slot.MaterialPath,
							Resources,
							Material,
							OutError))
					|| !TryBuildProjectionObject(
						{
							{TEXT("slot_name"), Name.ToSharedRef()},
							{TEXT("material_ref"), Material.ToSharedRef()},
						},
						Resources,
						Item,
						OutError))
				{
					return false;
				}
				Items.Add(Item.ToSharedRef());
			}
			return TryBuildProjectionArray(
				MoveTemp(Items),
				Resources,
				Out,
				OutError);
		}

		TArray<FProjectedMaterialSlot> CaptureProjectedMaterials(
			const UStaticMesh& Mesh)
		{
			TArray<FProjectedMaterialSlot> Out;
			for (const FStaticMaterial& Material : Mesh.GetStaticMaterials())
			{
				FProjectedMaterialSlot Slot;
				Slot.Name = Material.MaterialSlotName;
				Slot.bNullMaterial = Material.MaterialInterface == nullptr;
				if (Material.MaterialInterface)
				{
					Slot.MaterialPath =
						Material.MaterialInterface->GetPathName();
				}
				Out.Add(MoveTemp(Slot));
			}
			return Out;
		}

		TArray<FProjectedMaterialSlot> CaptureProjectedMaterials(
			const USkeletalMesh& Mesh)
		{
			TArray<FProjectedMaterialSlot> Out;
			for (const FSkeletalMaterial& Material : Mesh.GetMaterials())
			{
				FProjectedMaterialSlot Slot;
				Slot.Name = Material.MaterialSlotName;
				Slot.bNullMaterial = Material.MaterialInterface == nullptr;
				if (Material.MaterialInterface)
				{
					Slot.MaterialPath =
						Material.MaterialInterface->GetPathName();
				}
				Out.Add(MoveTemp(Slot));
			}
			return Out;
		}

		bool TryReadProjectedMaterials(
			const FUeAgentCanonicalValue& Value,
			TArray<FProjectedMaterialSlot>& Out,
			FExecutionMutationError& OutError)
		{

            ON_SCOPE_EXIT
            {
                if (!OutError.Code.IsEmpty())
                {
                    OutError.SourceKind = TEXT("native_result");
                    OutError.Source = {};
                    OutError.SemanticPointer.Reset();
                }
            };
            if (Value.GetKind() != EUeAgentValueKind::Array)
                return Fail(OutError, ValueFailure(TEXT("asset_data_material_slots_invalid"),
                    TEXT("Captured material slots must be an array."), TEXT("materials"), &Value, TEXT("array"), TEXT("native_result")));
			Out.Reset();
			TSet<FName> Seen;
            for (int32 Index = 0; Index < Value.GetArrayItems().Num(); ++Index)
            {
                const auto& Item = Value.GetArrayItems()[Index];
                const FString Field = TEXT("materials/") + FString::FromInt(Index);
				const FUeAgentCanonicalValue* NameValue =
					FindObjectField(Item.Get(), TEXT("slot_name"));
				const FUeAgentCanonicalValue* MaterialValue =
					FindObjectField(Item.Get(), TEXT("material_ref"));
				FString Name;
				FProjectedMaterialSlot Slot;
                if (!TryRequireObjectFields(Item.Get(), {TEXT("slot_name"), TEXT("material_ref")}, Field, OutError)) return false;
                if (!TryReadString(*NameValue, Name, Field + TEXT("/slot_name"), OutError)) return false;
                if (Name.TrimStartAndEnd().IsEmpty())
                    return Fail(OutError, ValueFailure(TEXT("asset_data_material_slot_invalid"),
                        TEXT("The captured material slot name must not be blank."), Field + TEXT("/slot_name"), NameValue, TEXT("nonblank slot name"), TEXT("native_result")));
				Slot.Name = FName(*Name);
				if (Seen.Contains(Slot.Name))
				{
                    return Fail(OutError, JsonFailure(TEXT("asset_data_material_slot_duplicate"),
                        TEXT("The captured material slot name is duplicated."), Field + TEXT("/slot_name"),
                        MakeShared<FJsonValueString>(Name), MakeShared<FJsonValueString>(TEXT("unique slot name")), TEXT("native_result")));
				}
				if (MaterialValue->GetKind() == EUeAgentValueKind::Null)
				{
					Slot.bNullMaterial = true;
				}
                else
                {
                    if (!TryReadString(*MaterialValue, Slot.MaterialPath, Field + TEXT("/material_ref"), OutError)) return false;
                    Slot.bNullMaterial = false;
                }
				Seen.Add(Slot.Name);
				Out.Add(MoveTemp(Slot));
			}
			return true;
		}

		bool TryNormalizeRequestedMaterials(
			IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
			const FUeAgentCanonicalValue& Value,
			TArray<FProjectedMaterialSlot>& Out,
			FExecutionMutationError& OutError)
		{
			TArray<FMaterialSlotValue> Parsed;
			if (!TryReadMaterialSlots(Resolver, Value, Parsed, OutError))
			{
				return false;
			}
			Out.Reset();
			for (const FMaterialSlotValue& Item : Parsed)
			{
				FProjectedMaterialSlot Slot;
				Slot.Name = Item.SlotName;
				Slot.bNullMaterial = !Item.Material.IsValid();
				if (Item.Material.IsValid())
				{
					Slot.MaterialPath = Item.Material->GetPathName();
				}
				Out.Add(MoveTemp(Slot));
			}
			return true;
		}

		const TCHAR* CollisionTraceName(const ECollisionTraceFlag Value)
		{
			switch (Value)
			{
			case CTF_UseDefault:
				return TEXT("default");
			case CTF_UseSimpleAndComplex:
				return TEXT("use_simple_and_complex");
			case CTF_UseSimpleAsComplex:
				return TEXT("use_simple_as_complex");
			case CTF_UseComplexAsSimple:
				return TEXT("use_complex_as_simple");
			default:
				return nullptr;
			}
		}

		bool TryBuildProjectedCollision(
			const FKAggregateGeom& Geometry,
			const ECollisionTraceFlag Trace,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& Out,
			FExecutionMutationError& OutError)
		{
			const TCHAR* TraceName = CollisionTraceName(Trace);
			if (!TraceName)
			{
                return Fail(OutError, NativeFailure(TEXT("asset_data_collision_invalid"),
                    TEXT("The native collision trace mode is not supported."), TEXT("collision/trace_mode"),
                    MakeShared<FJsonValueNumber>(static_cast<int32>(Trace)),
                    MakeShared<FJsonValueString>(TEXT("CTF_UseDefault, CTF_UseSimpleAndComplex, CTF_UseSimpleAsComplex, or CTF_UseComplexAsSimple"))));
			}
			FUeAgentCanonicalValuePtr TraceValue;
			TArray<FUeAgentCanonicalValueRef> Boxes;
			TArray<FUeAgentCanonicalValueRef> Spheres;
			TArray<FUeAgentCanonicalValueRef> Capsules;
			if (!TryBuildProjectionString(
					TraceName,
					Resources,
					TraceValue,
					OutError))
			{
				return false;
			}
			for (const FKBoxElem& Box : Geometry.BoxElems)
			{
                const FString Field = TEXT("collision/boxes/") + FString::FromInt(Boxes.Num());
				FUeAgentCanonicalValuePtr Center;
				FUeAgentCanonicalValuePtr Extent;
				FUeAgentCanonicalValuePtr Rotation;
				FUeAgentCanonicalValuePtr Item;
				if (!TryBuildProjectedVector(Box.Center, Resources, Center, OutError, Field + TEXT("/center"))
					|| !TryBuildProjectedVector(
						FVector(Box.X, Box.Y, Box.Z) * 0.5,
						Resources,
						Extent,
						OutError, Field + TEXT("/extent"))
					|| !TryBuildProjectedRotator(
						Box.Rotation,
						Resources,
						Rotation,
						OutError, Field + TEXT("/rotation"))
					|| !TryBuildProjectionObject(
						{
							{TEXT("center"), Center.ToSharedRef()},
							{TEXT("extent"), Extent.ToSharedRef()},
							{TEXT("rotation"), Rotation.ToSharedRef()},
						},
						Resources,
						Item,
						OutError))
				{
					return false;
				}
				Boxes.Add(Item.ToSharedRef());
			}
			for (const FKSphereElem& Sphere : Geometry.SphereElems)
			{
                const FString Field = TEXT("collision/spheres/") + FString::FromInt(Spheres.Num());
				FUeAgentCanonicalValuePtr Center;
				FUeAgentCanonicalValuePtr Radius;
				FUeAgentCanonicalValuePtr Item;
				if (!TryBuildProjectedVector(
						Sphere.Center,
						Resources,
						Center,
						OutError, Field + TEXT("/center"))
					|| !TryBuildProjectionDecimal(
						Sphere.Radius,
						Resources,
						Radius,
						OutError, Field + TEXT("/radius"))
					|| !TryBuildProjectionObject(
						{
							{TEXT("center"), Center.ToSharedRef()},
							{TEXT("radius"), Radius.ToSharedRef()},
						},
						Resources,
						Item,
						OutError))
				{
					return false;
				}
				Spheres.Add(Item.ToSharedRef());
			}
			for (const FKSphylElem& Capsule : Geometry.SphylElems)
			{
                const FString Field = TEXT("collision/capsules/") + FString::FromInt(Capsules.Num());
				FUeAgentCanonicalValuePtr Center;
				FUeAgentCanonicalValuePtr Rotation;
				FUeAgentCanonicalValuePtr Radius;
				FUeAgentCanonicalValuePtr Length;
				FUeAgentCanonicalValuePtr Item;
				if (!TryBuildProjectedVector(
						Capsule.Center,
						Resources,
						Center,
						OutError, Field + TEXT("/center"))
					|| !TryBuildProjectedRotator(
						Capsule.Rotation,
						Resources,
						Rotation,
						OutError, Field + TEXT("/rotation"))
					|| !TryBuildProjectionDecimal(
						Capsule.Radius,
						Resources,
						Radius,
						OutError, Field + TEXT("/radius"))
					|| !TryBuildProjectionDecimal(
						Capsule.Length,
						Resources,
						Length,
						OutError, Field + TEXT("/length"))
					|| !TryBuildProjectionObject(
						{
							{TEXT("center"), Center.ToSharedRef()},
							{TEXT("rotation"), Rotation.ToSharedRef()},
							{TEXT("radius"), Radius.ToSharedRef()},
							{TEXT("length"), Length.ToSharedRef()},
						},
						Resources,
						Item,
						OutError))
				{
					return false;
				}
				Capsules.Add(Item.ToSharedRef());
			}
			FUeAgentCanonicalValuePtr BoxArray;
			FUeAgentCanonicalValuePtr SphereArray;
			FUeAgentCanonicalValuePtr CapsuleArray;
			return TryBuildProjectionArray(
					MoveTemp(Boxes),
					Resources,
					BoxArray,
					OutError)
				&& TryBuildProjectionArray(
					MoveTemp(Spheres),
					Resources,
					SphereArray,
					OutError)
				&& TryBuildProjectionArray(
					MoveTemp(Capsules),
					Resources,
					CapsuleArray,
					OutError)
				&& TryBuildProjectionObject(
					{
						{TEXT("trace_mode"), TraceValue.ToSharedRef()},
						{TEXT("boxes"), BoxArray.ToSharedRef()},
						{TEXT("spheres"), SphereArray.ToSharedRef()},
						{TEXT("capsules"), CapsuleArray.ToSharedRef()},
					},
					Resources,
					Out,
					OutError);
		}

		bool TryBuildProjectedSockets(
			const TArray<FSocketValue>& Sockets,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& Out,
			FExecutionMutationError& OutError)
		{
			TArray<FUeAgentCanonicalValueRef> Items;
			for (const FSocketValue& Socket : Sockets)
			{
                const FString Field = TEXT("sockets/") + FString::FromInt(Items.Num());
				FUeAgentCanonicalValuePtr Name;
				FUeAgentCanonicalValuePtr Location;
				FUeAgentCanonicalValuePtr Rotation;
				FUeAgentCanonicalValuePtr Scale;
				FUeAgentCanonicalValuePtr Item;
				if (!TryBuildProjectionString(
						Socket.Name.ToString(),
						Resources,
						Name,
						OutError)
					|| !TryBuildProjectedVector(
						Socket.Location,
						Resources,
						Location,
						OutError, Field + TEXT("/location"))
					|| !TryBuildProjectedRotator(
						Socket.Rotation,
						Resources,
						Rotation,
						OutError, Field + TEXT("/rotation"))
					|| !TryBuildProjectedVector(
						Socket.Scale,
						Resources,
						Scale,
						OutError, Field + TEXT("/scale"))
					|| !TryBuildProjectionObject(
						{
							{TEXT("name"), Name.ToSharedRef()},
							{TEXT("location"), Location.ToSharedRef()},
							{TEXT("rotation"), Rotation.ToSharedRef()},
							{TEXT("scale"), Scale.ToSharedRef()},
						},
						Resources,
						Item,
						OutError))
				{
					return false;
				}
				Items.Add(Item.ToSharedRef());
			}
			return TryBuildProjectionArray(
				MoveTemp(Items),
				Resources,
				Out,
				OutError);
		}

		bool TryBuildProjectedBuildSettings(
			const TArray<FMeshBuildSettings>& Settings,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& Out,
			FExecutionMutationError& OutError)
		{
			TArray<FUeAgentCanonicalValueRef> Lods;
			for (int32 Index = 0; Index < Settings.Num(); ++Index)
			{
				const FMeshBuildSettings& Value = Settings[Index];
				FUeAgentCanonicalValuePtr LodIndex;
				FUeAgentCanonicalValuePtr RecomputeNormals;
				FUeAgentCanonicalValuePtr RecomputeTangents;
				FUeAgentCanonicalValuePtr WeightedNormals;
				FUeAgentCanonicalValuePtr RemoveDegenerates;
				FUeAgentCanonicalValuePtr UseMikk;
				FUeAgentCanonicalValuePtr GenerateLightmapUvs;
				FUeAgentCanonicalValuePtr Item;
				if (!TryBuildProjectionInteger(
						FString::FromInt(Index),
						Resources,
						LodIndex,
						OutError)
					|| !TryBuildProjectionBoolean(
						Value.bRecomputeNormals,
						Resources,
						RecomputeNormals,
						OutError)
					|| !TryBuildProjectionBoolean(
						Value.bRecomputeTangents,
						Resources,
						RecomputeTangents,
						OutError)
					|| !TryBuildProjectionBoolean(
						Value.bComputeWeightedNormals,
						Resources,
						WeightedNormals,
						OutError)
					|| !TryBuildProjectionBoolean(
						Value.bRemoveDegenerates,
						Resources,
						RemoveDegenerates,
						OutError)
					|| !TryBuildProjectionBoolean(
						Value.bUseMikkTSpace,
						Resources,
						UseMikk,
						OutError)
					|| !TryBuildProjectionBoolean(
						Value.bGenerateLightmapUVs,
						Resources,
						GenerateLightmapUvs,
						OutError)
					|| !TryBuildProjectionObject(
						{
							{TEXT("lod_index"), LodIndex.ToSharedRef()},
							{TEXT("recompute_normals"), RecomputeNormals.ToSharedRef()},
							{TEXT("recompute_tangents"), RecomputeTangents.ToSharedRef()},
							{TEXT("compute_weighted_normals"), WeightedNormals.ToSharedRef()},
							{TEXT("remove_degenerates"), RemoveDegenerates.ToSharedRef()},
							{TEXT("use_mikk_tspace"), UseMikk.ToSharedRef()},
							{TEXT("generate_lightmap_uvs"), GenerateLightmapUvs.ToSharedRef()},
						},
						Resources,
						Item,
						OutError))
				{
					return false;
				}
				Lods.Add(Item.ToSharedRef());
			}
			FUeAgentCanonicalValuePtr LodArray;
			return TryBuildProjectionArray(
					MoveTemp(Lods),
					Resources,
					LodArray,
					OutError)
				&& TryBuildProjectionObject(
					{{TEXT("lods"), LodArray.ToSharedRef()}},
					Resources,
					Out,
					OutError);
		}

		bool TryReadProjectedBuildSettings(
			const FUeAgentCanonicalValue& Value,
			TArray<FMeshBuildSettings>& Out,
			FExecutionMutationError& OutError)
		{

            ON_SCOPE_EXIT
            {
                if (!OutError.Code.IsEmpty())
                {
                    OutError.SourceKind = TEXT("native_result");
                    OutError.Source = {};
                    OutError.SemanticPointer.Reset();
                }
            };
			const FUeAgentCanonicalValue* Lods =
				FindObjectField(Value, TEXT("lods"));
			if (!Lods || Lods->GetKind() != EUeAgentValueKind::Array)
			{
                return Fail(OutError, ValueFailure(TEXT("asset_data_build_settings_invalid"),
                    TEXT("Captured build settings must contain a LOD array."), TEXT("build_settings/lods"), Lods, TEXT("array"), TEXT("native_result")));
			}
			Out.SetNum(Lods->GetArrayItems().Num());
			TArray<FMeshBuildSettings> Defaults;
			Defaults.SetNum(Out.Num());
			return Out.IsEmpty()
				? true
				: TryReadBuildSettings(
					Value,
					Out.Num(),
					Defaults,
					Out,
					OutError);
		}

		bool TryBuildProjectedBounds(
			const FBoxSphereBounds& Bounds,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& Out,
			FExecutionMutationError& OutError)
		{
			const FBox Box = Bounds.GetBox();
			FUeAgentCanonicalValuePtr Min;
			FUeAgentCanonicalValuePtr Max;
			return TryBuildProjectedVector(Box.Min, Resources, Min, OutError, TEXT("bounds/min"))
				&& TryBuildProjectedVector(Box.Max, Resources, Max, OutError, TEXT("bounds/max"))
				&& TryBuildProjectionObject(
					{
						{TEXT("min"), Min.ToSharedRef()},
						{TEXT("max"), Max.ToSharedRef()},
					},
					Resources,
					Out,
					OutError);
		}

		bool TryBuildProjectedReference(
			const UObject* Object,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& Out,
			FExecutionMutationError& OutError)
		{
			return Object
				? TryBuildProjectionString(
					Object->GetPathName(),
					Resources,
					Out,
					OutError)
				: TryBuildProjectionNull(Resources, Out, OutError);
		}

		bool TryBuildProjectedLodSettings(
			const USkeletalMeshLODSettings* Settings,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& Out,
			FExecutionMutationError& OutError)
		{
			FUeAgentCanonicalValuePtr Ref;
			return TryBuildProjectedReference(
					Settings,
					Resources,
					Ref,
					OutError)
				&& TryBuildProjectionObject(
					{{TEXT("lod_settings_ref"), Ref.ToSharedRef()}},
					Resources,
					Out,
					OutError);
		}

		const TCHAR* StructuredProjectionTag(
			const EStructuredAssetKind Kind,
			const FString& FieldId)
		{
			if (Kind == EStructuredAssetKind::StaticMesh)
			{
				if (FieldId == TEXT("materials")) return TEXT("static_mesh_material_slots");
				if (FieldId == TEXT("collision")) return TEXT("static_mesh_collision_asset_local");
				if (FieldId == TEXT("sockets")) return TEXT("static_mesh_sockets_relative");
				if (FieldId == TEXT("build_settings")) return TEXT("static_mesh_build_settings");
			}
			else if (Kind == EStructuredAssetKind::DataTable)
			{
				if (FieldId == TEXT("row_struct_ref")) return TEXT("data_table_row_struct");
				if (FieldId == TEXT("rows")) return TEXT("data_table_typed_rows");
			}
			else if (Kind == EStructuredAssetKind::SkeletalMesh)
			{
				if (FieldId == TEXT("materials")) return TEXT("skeletal_mesh_material_slots");
				if (FieldId == TEXT("lod_settings")) return TEXT("skeletal_mesh_lod_settings");
				if (FieldId == TEXT("bounds")) return TEXT("skeletal_mesh_imported_bounds");
				if (FieldId == TEXT("skeleton_ref")) return TEXT("skeletal_mesh_skeleton");
			}
			return nullptr;
		}

		bool TryBuildStructuredProjectionMaps(
			const EStructuredAssetKind Kind,
			UObject* Target,
			FUeAgentResourceTracker& Resources,
			TMap<FString, FUeAgentCanonicalValueRef>& OutCurrent,
			TMap<FString, FUeAgentCanonicalValueRef>& OutDefaults,
			FExecutionMutationError& OutError)
		{

            ON_SCOPE_EXIT
            {
                if (!OutError.Code.IsEmpty())
                {
                    OutError.SourceKind = TEXT("native_result");
                    if (Target && OutError.TargetPath.IsEmpty()) OutError.TargetPath = Target->GetPathName();
                    OutError.Source = {};
                    OutError.SemanticPointer.Reset();
                }
            };
			OutCurrent.Reset();
			OutDefaults.Reset();
			if (Kind == EStructuredAssetKind::StaticMesh)
			{
				UStaticMesh* Mesh = Cast<UStaticMesh>(Target);
				UBodySetup* BodySetup = Mesh ? Mesh->GetBodySetup() : nullptr;
                if (!Mesh)
                    return Fail(OutError, NativeFailure(TEXT("asset_data_static_mesh_invalid"),
                        TEXT("The projection target must be a static mesh."), TEXT("target"), ObjectClassValue(Target), MakeShared<FJsonValueString>(UStaticMesh::StaticClass()->GetPathName())));
                if (!BodySetup)
                    return Fail(OutError, NativeFailure(TEXT("asset_data_static_mesh_invalid"),
                        TEXT("The static mesh has no body setup."), TEXT("body_setup"), MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("available body setup"))));
                if (Mesh->GetNumSourceModels() <= 0)
                    return Fail(OutError, NativeFailure(TEXT("asset_data_static_mesh_invalid"),
                        TEXT("The static mesh has no source models."), TEXT("build_settings/lods"),
                        MakeShared<FJsonValueNumber>(Mesh->GetNumSourceModels()), MakeShared<FJsonValueString>(TEXT("one or more source models"))));
				TArray<FMeshBuildSettings> CurrentBuild;
				TArray<FMeshBuildSettings> DefaultBuild;
				for (int32 Index = 0; Index < Mesh->GetNumSourceModels(); ++Index)
				{
					CurrentBuild.Add(Mesh->GetSourceModel(Index).BuildSettings);
					DefaultBuild.Add(FMeshBuildSettings());
				}
				FUeAgentCanonicalValuePtr CurrentMaterials;
				FUeAgentCanonicalValuePtr DefaultMaterials;
				FUeAgentCanonicalValuePtr CurrentCollision;
				FUeAgentCanonicalValuePtr DefaultCollision;
				FUeAgentCanonicalValuePtr CurrentSockets;
				FUeAgentCanonicalValuePtr DefaultSockets;
				FUeAgentCanonicalValuePtr CurrentBuildValue;
				FUeAgentCanonicalValuePtr DefaultBuildValue;
				if (!TryBuildProjectedMaterials(
						CaptureProjectedMaterials(*Mesh),
						Resources,
						CurrentMaterials,
						OutError)
					|| !TryBuildProjectedMaterials(
						{},
						Resources,
						DefaultMaterials,
						OutError)
					|| !TryBuildProjectedCollision(
						BodySetup->AggGeom,
						BodySetup->CollisionTraceFlag,
						Resources,
						CurrentCollision,
						OutError)
					|| !TryBuildProjectedCollision(
						FKAggregateGeom(),
						CTF_UseDefault,
						Resources,
						DefaultCollision,
						OutError)
					|| !TryBuildProjectedSockets(
						CaptureSockets(*Mesh),
						Resources,
						CurrentSockets,
						OutError)
					|| !TryBuildProjectedSockets(
						{},
						Resources,
						DefaultSockets,
						OutError)
					|| !TryBuildProjectedBuildSettings(
						CurrentBuild,
						Resources,
						CurrentBuildValue,
						OutError)
					|| !TryBuildProjectedBuildSettings(
						DefaultBuild,
						Resources,
						DefaultBuildValue,
						OutError))
				{
					return false;
				}
				OutCurrent.Add(TEXT("materials"), CurrentMaterials.ToSharedRef());
				OutCurrent.Add(TEXT("collision"), CurrentCollision.ToSharedRef());
				OutCurrent.Add(TEXT("sockets"), CurrentSockets.ToSharedRef());
				OutCurrent.Add(TEXT("build_settings"), CurrentBuildValue.ToSharedRef());
				OutDefaults.Add(TEXT("materials"), DefaultMaterials.ToSharedRef());
				OutDefaults.Add(TEXT("collision"), DefaultCollision.ToSharedRef());
				OutDefaults.Add(TEXT("sockets"), DefaultSockets.ToSharedRef());
				OutDefaults.Add(TEXT("build_settings"), DefaultBuildValue.ToSharedRef());
				return true;
			}
			if (Kind == EStructuredAssetKind::DataTable)
			{
				UDataTable* Table = Cast<UDataTable>(Target);
				UScriptStruct* RowStruct = Table
					? const_cast<UScriptStruct*>(Table->GetRowStruct())
					: nullptr;
                if (!Table)
                    return Fail(OutError, NativeFailure(TEXT("asset_data_data_table_row_struct_invalid"),
                        TEXT("The projection target must be a DataTable."), TEXT("target"), ObjectClassValue(Target), MakeShared<FJsonValueString>(UDataTable::StaticClass()->GetPathName())));
                if (!FDataTableEditorUtils::IsValidTableStruct(RowStruct))
                    return Fail(OutError, NativeFailure(TEXT("asset_data_data_table_row_struct_invalid"),
                        TEXT("The DataTable row structure is not valid for projection."), TEXT("row_struct_ref"),
                        RowStruct ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(RowStruct->GetPathName())) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()),
                        MakeShared<FJsonValueString>(TEXT("valid DataTable row structure"))));
				FUeAgentCanonicalValuePtr RowStructValue;
				FUeAgentCanonicalValuePtr RowsValue;
				if (!TryBuildProjectionString(
						RowStruct->GetPathName(),
						Resources,
						RowStructValue,
						OutError)
					|| !TryBuildProjectedRows(
						RowStruct,
						CaptureRows(*Table, RowStruct),
						Resources,
						RowsValue,
						OutError))
				{
					return false;
				}
				OutCurrent.Add(TEXT("row_struct_ref"), RowStructValue.ToSharedRef());
				OutCurrent.Add(TEXT("rows"), RowsValue.ToSharedRef());
				OutDefaults = OutCurrent;
				return true;
			}

			USkeletalMesh* Mesh = Cast<USkeletalMesh>(Target);
			if (!Mesh)
			{
                return Fail(OutError, NativeFailure(TEXT("asset_data_skeletal_mesh_invalid"),
                    TEXT("The projection target must be a skeletal mesh."), TEXT("target"),
                    ObjectClassValue(Target), MakeShared<FJsonValueString>(USkeletalMesh::StaticClass()->GetPathName())));
			}
			FUeAgentCanonicalValuePtr Materials;
			FUeAgentCanonicalValuePtr LodSettings;
			FUeAgentCanonicalValuePtr Bounds;
			FUeAgentCanonicalValuePtr Skeleton;
			if (!TryBuildProjectedMaterials(
					CaptureProjectedMaterials(*Mesh),
					Resources,
					Materials,
					OutError)
				|| !TryBuildProjectedLodSettings(
					Mesh->GetLODSettings(),
					Resources,
					LodSettings,
					OutError)
				|| !TryBuildProjectedBounds(
					Mesh->GetImportedBounds(),
					Resources,
					Bounds,
					OutError)
				|| !TryBuildProjectedReference(
					Mesh->GetSkeleton(),
					Resources,
					Skeleton,
					OutError))
			{
				return false;
			}
			OutCurrent.Add(TEXT("materials"), Materials.ToSharedRef());
			OutCurrent.Add(TEXT("lod_settings"), LodSettings.ToSharedRef());
			OutCurrent.Add(TEXT("bounds"), Bounds.ToSharedRef());
			OutCurrent.Add(TEXT("skeleton_ref"), Skeleton.ToSharedRef());
			OutDefaults = OutCurrent;
			return true;
		}

		bool TrySelectStructuredProjectionFields(
			const EStructuredAssetKind Kind,
			const TArray<FString>& SelectedFieldIds,
			const TMap<FString, FUeAgentCanonicalValueRef>& Current,
			const TMap<FString, FUeAgentCanonicalValueRef>& Defaults,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutCurrent,
			FUeAgentCanonicalValuePtr& OutDefaults,
			FUeAgentCanonicalValuePtr& OutContext,
			FExecutionMutationError& OutError)
		{
			if (SelectedFieldIds.IsEmpty())
			{
                return Fail(OutError, JsonFailure(TEXT("asset_data_structured_asset_selected_fields_missing"),
                    TEXT("At least one field must be selected."), TEXT("selected_fields"),
                    MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("one or more selected fields"))));
			}
			TSet<FString> Seen;
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> CurrentItems;
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> DefaultItems;
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> ContextItems;
            for (int32 Index = 0; Index < SelectedFieldIds.Num(); ++Index)
            {
                const FString& FieldId = SelectedFieldIds[Index];
				const FUeAgentCanonicalValueRef* CurrentValue =
					Current.Find(FieldId);
				const FUeAgentCanonicalValueRef* DefaultValue =
					Defaults.Find(FieldId);
				const TCHAR* Tag = StructuredProjectionTag(Kind, FieldId);
				FUeAgentCanonicalValuePtr ContextValue;
				if (Seen.Contains(FieldId)
					|| !CurrentValue
					|| !DefaultValue
					|| !Tag)
				{
                    return Fail(OutError, JsonFailure(TEXT("asset_data_structured_asset_selected_field_invalid"),
                        Seen.Contains(FieldId) ? TEXT("The field is selected more than once.")
                            : !CurrentValue ? TEXT("The selected field has no current projection.")
                                : !DefaultValue ? TEXT("The selected field has no default projection.")
                                    : TEXT("The selected field has no registered projection."),
                        TEXT("selected_fields/") + FString::FromInt(Index),
                        MakeShared<FJsonValueString>(FieldId), MakeShared<FJsonValueString>(TEXT("unique available field"))));
				}
				if (!TryBuildProjectionString(Tag, Resources, ContextValue, OutError)) return false;
				Seen.Add(FieldId);
				CurrentItems.Emplace(FieldId, *CurrentValue);
				DefaultItems.Emplace(FieldId, *DefaultValue);
				ContextItems.Emplace(FieldId, ContextValue.ToSharedRef());
			}
			return TryBuildProjectionObject(
					MoveTemp(CurrentItems),
					Resources,
					OutCurrent,
					OutError)
				&& TryBuildProjectionObject(
					MoveTemp(DefaultItems),
					Resources,
					OutDefaults,
					OutError)
				&& TryBuildProjectionObject(
					MoveTemp(ContextItems),
					Resources,
					OutContext,
					OutError);
		}

		struct FProjectedChange final
		{
			FString Presence;
			const FUeAgentCanonicalValue* Requested = nullptr;
		};

        bool TryReadStructuredChanges(const EStructuredAssetKind Kind,
            const FUeAgentCanonicalValue& CurrentFields, const FUeAgentCanonicalValue& DefaultFields,
            const FUeAgentCanonicalValue& ProjectionContext, const FUeAgentCanonicalValue& ProposedAfter,
            TMap<FString, FProjectedChange>& Out, FExecutionMutationError& OutError)
        {
            Out.Reset();
            const FUeAgentCanonicalValue* TypedChanges = FindObjectField(ProposedAfter, TEXT("typed_changes"));
            const FUeAgentCanonicalValue* Captured[] = {&CurrentFields, &DefaultFields, &ProjectionContext};
            const TCHAR* CapturedNames[] = {TEXT("current_fields"), TEXT("default_fields"), TEXT("projection_context")};
            for (int32 Index = 0; Index < 3; ++Index)
                if (Captured[Index]->GetKind() != EUeAgentValueKind::Object)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_structured_asset_expected_state_invalid"),
                        TEXT("The captured field projection must be an object."), CapturedNames[Index], Captured[Index], TEXT("object"), TEXT("native_result")));
            const int32 Count = CurrentFields.GetObjectItems().Num();
            for (int32 Index = 1; Index < 3; ++Index)
                if (Captured[Index]->GetObjectItems().Num() != Count)
                    return Fail(OutError, NativeFailure(TEXT("asset_data_structured_asset_expected_state_invalid"),
                        TEXT("The captured projection field count does not match the current fields."), CapturedNames[Index],
                        MakeShared<FJsonValueNumber>(Captured[Index]->GetObjectItems().Num()), MakeShared<FJsonValueNumber>(Count)));
            if (!TypedChanges || TypedChanges->GetKind() != EUeAgentValueKind::Array)
                return Fail(OutError, ValueFailure(TEXT("asset_data_structured_asset_expected_state_invalid"),
                    TEXT("Typed changes must be an array."), TEXT("typed_changes"), TypedChanges, TEXT("array")));
            if (TypedChanges->GetArrayItems().Num() != Count)
                return Fail(OutError, JsonFailure(TEXT("asset_data_structured_asset_expected_state_invalid"),
                    TEXT("The number of changes must match the selected fields."), TEXT("typed_changes"),
                    MakeShared<FJsonValueNumber>(TypedChanges->GetArrayItems().Num()), MakeShared<FJsonValueNumber>(Count)));
            for (int32 Index = 0; Index < TypedChanges->GetArrayItems().Num(); ++Index)
            {
                const auto& ChangeValue = TypedChanges->GetArrayItems()[Index];
                const FString Path = TEXT("typed_changes/") + FString::FromInt(Index);
                const FUeAgentCanonicalValue* Field = FindObjectField(*ChangeValue, TEXT("field_id"));
                const FUeAgentCanonicalValue* Presence = FindObjectField(*ChangeValue, TEXT("presence"));
                FString FieldId;
                FProjectedChange Change;
                if (!Field || !Presence)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_structured_asset_expected_change_invalid"),
                        TEXT("The change is missing a required field."), Path + (!Field ? TEXT("/field_id") : TEXT("/presence")), nullptr, TEXT("string")));
                if (!TryReadString(*Field, FieldId, Path + TEXT("/field_id"), OutError)
                    || !TryReadString(*Presence, Change.Presence, Path + TEXT("/presence"), OutError)) return false;
                if (Out.Contains(FieldId) || !FindObjectField(CurrentFields, FieldId))
                    return Fail(OutError, JsonFailure(TEXT("asset_data_structured_asset_expected_change_invalid"),
                        Out.Contains(FieldId) ? TEXT("The field change is duplicated.") : TEXT("The change refers to an unselected field."),
                        Path + TEXT("/field_id"), MakeShared<FJsonValueString>(FieldId), MakeShared<FJsonValueString>(TEXT("unique selected field"))));
                const FUeAgentCanonicalValue* Context = FindObjectField(ProjectionContext, FieldId);
                const TCHAR* ExpectedTag = StructuredProjectionTag(Kind, FieldId);
                if (!Context || !ExpectedTag || Context->GetKind() != EUeAgentValueKind::String || Context->GetText() != ExpectedTag)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_structured_asset_projection_context_invalid"),
                        !Context ? TEXT("The captured field projection context is missing.")
                            : !ExpectedTag ? TEXT("The field has no registered projection context.")
                                : TEXT("The captured projection context does not match the field."),
                        ChildPath(TEXT("projection_context"), FieldId), Context,
                        ExpectedTag ? FString(ExpectedTag) : TEXT("registered field projection"), TEXT("native_result")));
                const FUeAgentCanonicalValue* TypedValue = FindObjectField(*ChangeValue, TEXT("value"));
                Change.Requested = TypedValue ? FindObjectField(*TypedValue, TEXT("canonical_value")) : nullptr;
                if (Change.Presence != TEXT("clear") && !Change.Requested)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_structured_asset_expected_value_missing"),
                        TEXT("The field operation requires a canonical value."), FieldId, nullptr, TEXT("canonical value")));
                Out.Add(FieldId, MoveTemp(Change));
            }
            return true;
        }

		bool TryBuildExpectedStaticFields(
			IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
			const FUeAgentCanonicalValue& CurrentFields,
			const FUeAgentCanonicalValue& DefaultFields,
			const TMap<FString, FProjectedChange>& Changes,
			FUeAgentResourceTracker& Resources,
			TMap<FString, FUeAgentCanonicalValueRef>& Out,
			FExecutionMutationError& OutError)
		{
			Out.Reset();
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Current
				: CurrentFields.GetObjectItems())
			{
				const FProjectedChange* Change = Changes.Find(Current.Key);
				const FUeAgentCanonicalValueRef* Default =
					FindObjectFieldRef(DefaultFields, Current.Key);
                if (!Change)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_structured_asset_expected_change_invalid"),
                        TEXT("The selected field has no change."), Current.Key, nullptr, TEXT("field change")));
                if (!Default)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_structured_asset_expected_change_invalid"),
                        TEXT("The selected field has no captured default."), ChildPath(TEXT("default_fields"), Current.Key), nullptr, TEXT("captured default"), TEXT("native_result")));
				if (Change->Presence == TEXT("clear"))
				{
					Out.Add(Current.Key, *Default);
					continue;
				}
				FUeAgentCanonicalValuePtr Expected;
				if (Current.Key == TEXT("materials"))
				{
					TArray<FProjectedMaterialSlot> Slots;
					if (Change->Presence == TEXT("replace"))
					{
						if (!TryNormalizeRequestedMaterials(
								Resolver,
								*Change->Requested,
								Slots,
								OutError))
						{
							return false;
						}
					}
					else if (Change->Presence == TEXT("set"))
					{
						TArray<FProjectedMaterialSlot> Updates;
						if (!TryReadProjectedMaterials(Current.Value.Get(), Slots, OutError)
							|| !TryNormalizeRequestedMaterials(
								Resolver,
								*Change->Requested,
								Updates,
								OutError))
						{
							return false;
						}
						for (FProjectedMaterialSlot& Update : Updates)
						{
							const int32 Index = Slots.IndexOfByPredicate(
								[&Update](const FProjectedMaterialSlot& Item)
								{
									return Item.Name == Update.Name;
								});
							if (Index == INDEX_NONE)
							{
								Slots.Add(MoveTemp(Update));
							}
							else
							{
								Slots[Index] = MoveTemp(Update);
							}
						}
					}
					else if (Change->Presence == TEXT("remove"))
					{
						TSet<FName> Names;
						if (!TryReadProjectedMaterials(Current.Value.Get(), Slots, OutError)
							|| !TryReadRemovalNames(
								*Change->Requested,
								Names,
								OutError,
								TEXT("asset_data_material_remove_invalid"), TEXT("materials")))
						{
							return false;
						}
						Slots.RemoveAll(
							[&Names](const FProjectedMaterialSlot& Item)
							{
								return Names.Contains(Item.Name);
							});
					}
					else
					{
                        return Fail(OutError, JsonFailure(TEXT("asset_data_material_presence_invalid"),
                            TEXT("The material operation is not supported."), TEXT("materials/presence"),
                            MakeShared<FJsonValueString>(Change->Presence), MakeShared<FJsonValueString>(TEXT("clear, set, replace, or remove"))));
					}
					if (!TryBuildProjectedMaterials(
							Slots,
							Resources,
							Expected,
							OutError))
					{
						return false;
					}
				}
				else if (Current.Key == TEXT("collision"))
				{
					FKAggregateGeom Geometry;
					ECollisionTraceFlag Trace = CTF_UseDefault;
                    if (!TryRequireChange(TEXT("collision"), true, Change->Presence, Change->Requested, TEXT("replace"), OutError)) return false;
                    if (!TryReadCollision(
							*Change->Requested,
							Geometry,
							Trace,
							OutError)
						|| !TryBuildProjectedCollision(
							Geometry,
							Trace,
							Resources,
							Expected,
							OutError))
					{
						return false;
					}
				}
				else if (Current.Key == TEXT("sockets"))
				{
					TArray<FSocketValue> CurrentSockets;
					TArray<FSocketValue> ExpectedSockets;
					FStructuredAssetChangeView View{
						Current.Key,
						Change->Presence,
						Change->Requested,
					};
                    if (!TryReadSockets(Current.Value.Get(), CurrentSockets, OutError))
                    {
                        OutError.SourceKind = TEXT("native_result");
                        OutError.Source = {};
                        OutError.SemanticPointer.Reset();
                        return false;
                    }
                    if (!TryApplySocketPresence(
							View,
							CurrentSockets,
							ExpectedSockets,
							OutError)
						|| !TryBuildProjectedSockets(
							ExpectedSockets,
							Resources,
							Expected,
							OutError))
					{
						return false;
					}
				}
				else if (Current.Key == TEXT("build_settings"))
				{
					TArray<FMeshBuildSettings> CurrentSettings;
					TArray<FMeshBuildSettings> ExpectedSettings;
                    if (!TryRequireChange(TEXT("build_settings"), true, Change->Presence, Change->Requested, TEXT("set"), OutError)) return false;
                    if (!TryReadProjectedBuildSettings(
							Current.Value.Get(),
							CurrentSettings,
							OutError)
						|| !TryReadBuildSettings(
							*Change->Requested,
							CurrentSettings.Num(),
							CurrentSettings,
							ExpectedSettings,
							OutError)
						|| !TryBuildProjectedBuildSettings(
							ExpectedSettings,
							Resources,
							Expected,
							OutError))
					{
						return false;
					}
				}
				else
				{
                    return Fail(OutError, JsonFailure(TEXT("asset_data_structured_asset_expected_field_invalid"),
                        TEXT("The captured field is not supported for static mesh projection."), TEXT("current_fields"),
                        MakeShared<FJsonValueString>(Current.Key), MakeShared<FJsonValueString>(TEXT("materials, collision, sockets, or build_settings")), TEXT("native_result")));
				}
				Out.Add(Current.Key, Expected.ToSharedRef());
			}
			return true;
		}

		bool TryBuildExpectedDataTableFields(
			IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
			const TMap<FString, FProjectedChange>& Changes,
			FUeAgentResourceTracker& Resources,
			TMap<FString, FUeAgentCanonicalValueRef>& Out,
			FExecutionMutationError& OutError)
		{
			Out.Reset();
			const FProjectedChange* RowStructChange =
				Changes.Find(TEXT("row_struct_ref"));
			const FProjectedChange* RowsChange = Changes.Find(TEXT("rows"));
			FString RowStructPath;
            if (Changes.Num() != 2)
                return Fail(OutError, JsonFailure(TEXT("asset_data_data_table_changes_invalid"),
                    TEXT("A DataTable update requires two field changes."), TEXT("typed_changes"),
                    MakeShared<FJsonValueNumber>(Changes.Num()), MakeShared<FJsonValueNumber>(2)));
            if (!TryRequireChange(TEXT("row_struct_ref"), RowStructChange != nullptr,
                    RowStructChange ? RowStructChange->Presence : FString(), RowStructChange ? RowStructChange->Requested : nullptr, TEXT("replace"), OutError)
                || !TryReadString(*RowStructChange->Requested, RowStructPath, TEXT("row_struct_ref"), OutError)
                || !TryRequireChange(TEXT("rows"), RowsChange != nullptr,
                    RowsChange ? RowsChange->Presence : FString(), RowsChange ? RowsChange->Requested : nullptr, TEXT("replace"), OutError)) return false;
			UScriptStruct* RowStruct = LoadObject<UScriptStruct>(
				nullptr,
				*RowStructPath);
			if (!FDataTableEditorUtils::IsValidTableStruct(RowStruct))
			{
                return Fail(OutError, JsonFailure(TEXT("asset_data_data_table_row_struct_invalid"),
                    TEXT("The reference did not load a valid DataTable row structure."), TEXT("row_struct_ref"),
                    MakeShared<FJsonValueString>(RowStructPath), MakeShared<FJsonValueString>(TEXT("loadable valid DataTable row structure"))));
			}
			TArray<FTableRowValue> Rows;
			FUeAgentCanonicalValuePtr ProjectedRowStruct;
			FUeAgentCanonicalValuePtr ProjectedRows;
			if (!TryReadRows(
					Resolver,
					RowStruct,
					*RowsChange->Requested,
					Rows,
					OutError)
				|| !TryBuildProjectionString(
					RowStruct->GetPathName(),
					Resources,
					ProjectedRowStruct,
					OutError)
				|| !TryBuildProjectedRows(
					RowStruct,
					Rows,
					Resources,
					ProjectedRows,
					OutError))
			{
				return false;
			}
			Out.Add(TEXT("row_struct_ref"), ProjectedRowStruct.ToSharedRef());
			Out.Add(TEXT("rows"), ProjectedRows.ToSharedRef());
			return true;
		}

		bool TryBuildExpectedSkeletalFields(
			IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
			const TMap<FString, FProjectedChange>& Changes,
			FUeAgentResourceTracker& Resources,
			TMap<FString, FUeAgentCanonicalValueRef>& Out,
			FExecutionMutationError& OutError)
		{
			Out.Reset();
			const FProjectedChange* Materials = Changes.Find(TEXT("materials"));
			const FProjectedChange* LodSettings = Changes.Find(TEXT("lod_settings"));
			const FProjectedChange* Bounds = Changes.Find(TEXT("bounds"));
			const FProjectedChange* Skeleton = Changes.Find(TEXT("skeleton_ref"));
            if (Changes.Num() != 4)
                return Fail(OutError, JsonFailure(TEXT("asset_data_skeletal_mesh_changes_invalid"),
                    TEXT("A skeletal mesh update requires four field changes."), TEXT("typed_changes"),
                    MakeShared<FJsonValueNumber>(Changes.Num()), MakeShared<FJsonValueNumber>(4)));
            if (!TryRequireChange(TEXT("materials"), Materials != nullptr, Materials ? Materials->Presence : FString(), Materials ? Materials->Requested : nullptr, TEXT("replace"), OutError)
                || !TryRequireChange(TEXT("lod_settings"), LodSettings != nullptr, LodSettings ? LodSettings->Presence : FString(), LodSettings ? LodSettings->Requested : nullptr, TEXT("replace"), OutError)
                || !TryRequireChange(TEXT("bounds"), Bounds != nullptr, Bounds ? Bounds->Presence : FString(), Bounds ? Bounds->Requested : nullptr, TEXT("replace"), OutError)
                || !TryRequireChange(TEXT("skeleton_ref"), Skeleton != nullptr, Skeleton ? Skeleton->Presence : FString(), Skeleton ? Skeleton->Requested : nullptr, TEXT("replace"), OutError)) return false;
			TArray<FProjectedMaterialSlot> Slots;
			if (!TryNormalizeRequestedMaterials(
					Resolver,
					*Materials->Requested,
					Slots,
					OutError)
                || !TryRequireObjectFields(*LodSettings->Requested, {TEXT("lod_settings_ref")}, TEXT("lod_settings"), OutError))
			{
				return false;
			}
			FString LodSettingsRef;
			FString SkeletonRef;
            if (!TryReadString(*FindObjectField(*LodSettings->Requested, TEXT("lod_settings_ref")), LodSettingsRef, TEXT("lod_settings/lod_settings_ref"), OutError)
                || !TryReadString(*Skeleton->Requested, SkeletonRef, TEXT("skeleton_ref"), OutError)) return false;
			UObject* LodSettingsObject = nullptr;
			UObject* SkeletonObject = nullptr;
			FBoxSphereBounds ParsedBounds;
            if (!TryResolveStructuredReference(Resolver, LodSettingsRef, LodSettingsObject, TEXT("lod_settings/lod_settings_ref"), OutError)
                || !TryResolveStructuredReference(Resolver, SkeletonRef, SkeletonObject, TEXT("skeleton_ref"), OutError)) return false;
            if (!Cast<USkeletalMeshLODSettings>(LodSettingsObject))
                return Fail(OutError, ReferenceClassFailure(TEXT("lod_settings/lod_settings_ref"), LodSettingsRef, LodSettingsObject, *USkeletalMeshLODSettings::StaticClass()));
            if (!Cast<USkeleton>(SkeletonObject))
                return Fail(OutError, ReferenceClassFailure(TEXT("skeleton_ref"), SkeletonRef, SkeletonObject, *USkeleton::StaticClass()));
            if (!TryReadBounds(*Bounds->Requested, ParsedBounds, OutError)) return false;
			FUeAgentCanonicalValuePtr ProjectedMaterials;
			FUeAgentCanonicalValuePtr ProjectedLodSettings;
			FUeAgentCanonicalValuePtr ProjectedBounds;
			FUeAgentCanonicalValuePtr ProjectedSkeleton;
			if (!TryBuildProjectedMaterials(
					Slots,
					Resources,
					ProjectedMaterials,
					OutError)
				|| !TryBuildProjectedLodSettings(
					CastChecked<USkeletalMeshLODSettings>(LodSettingsObject),
					Resources,
					ProjectedLodSettings,
					OutError)
				|| !TryBuildProjectedBounds(
					ParsedBounds,
					Resources,
					ProjectedBounds,
					OutError)
				|| !TryBuildProjectedReference(
					CastChecked<USkeleton>(SkeletonObject),
					Resources,
					ProjectedSkeleton,
					OutError))
			{
				return false;
			}
			Out.Add(TEXT("materials"), ProjectedMaterials.ToSharedRef());
			Out.Add(TEXT("lod_settings"), ProjectedLodSettings.ToSharedRef());
			Out.Add(TEXT("bounds"), ProjectedBounds.ToSharedRef());
			Out.Add(TEXT("skeleton_ref"), ProjectedSkeleton.ToSharedRef());
			return true;
		}
	}

	bool IsStructuredAssetMechanicId(
		const FString& NativeMechanicId)
	{
		return FindStructuredAssetKind(NativeMechanicId) != nullptr;
	}

	bool TryProjectStructuredAssetCurrentFieldState(
		IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
		const FString& NativeMechanicId,
		UObject* Target,
		const TArray<FString>& SelectedFieldIds,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutCurrentFields,
		FUeAgentCanonicalValuePtr& OutDefaultFields,
		FUeAgentCanonicalValuePtr& OutProjectionContext,
		FExecutionMutationError& OutError)
	{
		(void)Resolver;
		OutCurrentFields.Reset();
		OutDefaultFields.Reset();
		OutProjectionContext.Reset();
		const EStructuredAssetKind* Kind =
			FindStructuredAssetKind(NativeMechanicId);
        if (!Kind) return Fail(OutError, UnsupportedMechanic(NativeMechanicId));
        if (!Target)
            return Fail(OutError, NativeFailure(TEXT("asset_data_structured_asset_projection_unsupported"),
                TEXT("The structured asset projection has no target."), TEXT("target"),
                MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("available target asset"))));
		TMap<FString, FUeAgentCanonicalValueRef> Current;
		TMap<FString, FUeAgentCanonicalValueRef> Defaults;
		return TryBuildStructuredProjectionMaps(
				*Kind,
				Target,
				Resources,
				Current,
				Defaults,
				OutError)
			&& TrySelectStructuredProjectionFields(
				*Kind,
				SelectedFieldIds,
				Current,
				Defaults,
				Resources,
				OutCurrentFields,
				OutDefaultFields,
				OutProjectionContext,
				OutError);
	}

	bool TryProjectStructuredAssetExpectedFieldState(
		IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
		const FString& NativeMechanicId,
		const FUeAgentCanonicalValue& CurrentFields,
		const FUeAgentCanonicalValue& DefaultFields,
		const FUeAgentCanonicalValue& ProjectionContext,
		const FUeAgentCanonicalValue& ProposedAfter,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutExpectedFields,
		FExecutionMutationError& OutError)
	{
		OutExpectedFields.Reset();
		const EStructuredAssetKind* Kind =
			FindStructuredAssetKind(NativeMechanicId);
        if (!Kind) return Fail(OutError, UnsupportedMechanic(NativeMechanicId));
		TMap<FString, FProjectedChange> Changes;
		if (!TryReadStructuredChanges(
				*Kind,
				CurrentFields,
				DefaultFields,
				ProjectionContext,
				ProposedAfter,
				Changes,
				OutError))
		{
			return false;
		}
		TMap<FString, FUeAgentCanonicalValueRef> Expected;
		const bool bBuilt =
			*Kind == EStructuredAssetKind::StaticMesh
				? TryBuildExpectedStaticFields(
					Resolver,
					CurrentFields,
					DefaultFields,
					Changes,
					Resources,
					Expected,
					OutError)
				: *Kind == EStructuredAssetKind::DataTable
					? TryBuildExpectedDataTableFields(
						Resolver,
						Changes,
						Resources,
						Expected,
						OutError)
					: TryBuildExpectedSkeletalFields(
						Resolver,
						Changes,
						Resources,
						Expected,
						OutError);
        if (!bBuilt) return false;
        if (Expected.Num() != CurrentFields.GetObjectItems().Num())
            return Fail(OutError, NativeFailure(TEXT("asset_data_structured_asset_expected_field_count_invalid"),
                TEXT("The constructed expected field count does not match the current fields."), TEXT("expected_fields"),
                MakeShared<FJsonValueNumber>(Expected.Num()), MakeShared<FJsonValueNumber>(CurrentFields.GetObjectItems().Num())));
		TArray<TPair<FString, FUeAgentCanonicalValueRef>> Items;
		for (const TPair<FString, FUeAgentCanonicalValueRef>& Current
			: CurrentFields.GetObjectItems())
		{
			const FUeAgentCanonicalValueRef* Value = Expected.Find(Current.Key);
			if (!Value)
			{
                return Fail(OutError, JsonFailure(TEXT("asset_data_structured_asset_expected_field_invalid"),
                    TEXT("The constructed expected state is missing a selected field."), TEXT("expected_fields"),
                    MakeShared<FJsonValueString>(Current.Key), MakeShared<FJsonValueString>(TEXT("expected value for selected field")), TEXT("native_result")));
			}
			Items.Emplace(Current.Key, *Value);
		}
		return TryBuildProjectionObject(
			MoveTemp(Items),
			Resources,
			OutExpectedFields,
			OutError);
	}

	struct FStructuredAssetPreparedData
	{
		EStructuredAssetKind Kind =
			EStructuredAssetKind::StaticMesh;

		bool bChangeStaticMaterials = false;
		bool bChangeStaticCollision = false;
		bool bChangeStaticSockets = false;
		bool bChangeStaticBuildSettings = false;
		TArray<FStaticMaterial> OriginalStaticMaterials;
		TArray<FStaticMaterial> NewStaticMaterials;
		TArray<int32> MaterialSources;
		TUniquePtr<UeAgentReflectionAuthoring::MeshMaterialRelations::FSnapshot> MaterialRelationsBefore;
		FKAggregateGeom OriginalStaticGeometry;
		FKAggregateGeom NewStaticGeometry;
		ECollisionTraceFlag OriginalStaticTrace = CTF_UseDefault;
		ECollisionTraceFlag NewStaticTrace = CTF_UseDefault;
		TArray<TObjectPtr<UStaticMeshSocket>>
			OriginalStaticSockets;
		TArray<FSocketValue> NewStaticSockets;
		TArray<FMeshBuildSettings>
			OriginalStaticBuildSettings;
		TArray<FMeshBuildSettings> NewStaticBuildSettings;

		TStrongObjectPtr<UScriptStruct> OriginalRowStruct;
		TStrongObjectPtr<UScriptStruct> NewRowStruct;
		TArray<FTableRowValue> OriginalRows;
		TArray<FTableRowValue> NewRows;

		TArray<FSkeletalMaterial> OriginalSkeletalMaterials;
		TArray<FSkeletalMaterial> NewSkeletalMaterials;
		TStrongObjectPtr<USkeletalMeshLODSettings>
			OriginalLodSettings;
		TStrongObjectPtr<USkeletalMeshLODSettings>
			NewLodSettings;
		FBoxSphereBounds OriginalSkeletalBounds;
		FBoxSphereBounds NewSkeletalBounds;
		TStrongObjectPtr<USkeleton> OriginalSkeleton;
		TStrongObjectPtr<USkeleton> NewSkeleton;
	};

	bool TryPrepareStructuredAssetUpdate(
		IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
		const FString& NativeMechanicId,
		UObject* Target,
		const TArray<FStructuredAssetChangeView>& Changes,
		FStructuredAssetPreparedState& OutState,
		FExecutionMutationError& OutError)
	{
        ON_SCOPE_EXIT
        {
            if (!OutError.Code.IsEmpty() && Target && OutError.TargetPath.IsEmpty()) OutError.TargetPath = Target->GetPathName();
        };
		OutState.Data.Reset();
		const EStructuredAssetKind* Kind =
			FindStructuredAssetKind(NativeMechanicId);
        if (!Kind) return Fail(OutError, UnsupportedMechanic(NativeMechanicId));
        if (!Target)
            return Fail(OutError, NativeFailure(TEXT("asset_data_structured_asset_target_missing"),
                TEXT("The structured asset update has no target."), TEXT("target"),
                MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("available target asset"))));
        if (Changes.IsEmpty())
            return Fail(OutError, JsonFailure(TEXT("asset_data_structured_asset_changes_missing"),
                TEXT("At least one field change is required."), TEXT("typed_changes"),
                MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("one or more changes"))));
		const TSharedRef<
			FStructuredAssetPreparedData,
			ESPMode::ThreadSafe> Data =
				MakeShared<
					FStructuredAssetPreparedData,
					ESPMode::ThreadSafe>();
		Data->Kind = *Kind;

		if (Data->Kind == EStructuredAssetKind::StaticMesh)
		{
			UStaticMesh* Mesh = Cast<UStaticMesh>(Target);
            if (!Mesh)
                return Fail(OutError, NativeFailure(TEXT("asset_data_static_mesh_invalid"),
                    TEXT("The update target must be a static mesh."), TEXT("target"), ObjectClassValue(Target), MakeShared<FJsonValueString>(UStaticMesh::StaticClass()->GetPathName())));
            if (!Mesh->GetBodySetup())
                return Fail(OutError, NativeFailure(TEXT("asset_data_static_mesh_invalid"),
                    TEXT("The static mesh has no body setup."), TEXT("body_setup"), MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("available body setup"))));
            if (Changes.Num() > 4)
                return Fail(OutError, JsonFailure(TEXT("asset_data_static_mesh_invalid"),
                    TEXT("A static mesh update supports at most four field changes."), TEXT("typed_changes"),
                    MakeShared<FJsonValueNumber>(Changes.Num()), MakeShared<FJsonValueNumber>(4)));
			Data->OriginalStaticMaterials =
				Mesh->GetStaticMaterials();
			Data->NewStaticMaterials =
				Data->OriginalStaticMaterials;
			Data->OriginalStaticGeometry =
				Mesh->GetBodySetup()->AggGeom;
			Data->NewStaticGeometry =
				Data->OriginalStaticGeometry;
			Data->OriginalStaticTrace =
				Mesh->GetBodySetup()->CollisionTraceFlag;
			Data->NewStaticTrace =
				Data->OriginalStaticTrace;
			Data->OriginalStaticSockets = Mesh->Sockets;
			Data->NewStaticSockets = CaptureSockets(*Mesh);
			for (int32 Index = 0;
				Index < Mesh->GetNumSourceModels();
				++Index)
			{
				Data->OriginalStaticBuildSettings.Add(
					Mesh->GetSourceModel(Index).BuildSettings);
			}
			Data->NewStaticBuildSettings =
				Data->OriginalStaticBuildSettings;

			if (const FStructuredAssetChangeView* Materials =
				FindChange(Changes, TEXT("materials")))
			{
				Data->bChangeStaticMaterials = true;
				TArray<FMaterialSlotValue> NewSlots;
				if (!TryApplyMaterialPresence(
						Resolver,
						*Materials,
						CaptureStaticMaterialSlots(*Mesh),
						NewSlots,
						OutError))
				{
					return false;
				}
				Data->NewStaticMaterials =
					MakeStaticMaterials(NewSlots);
				Data->MaterialRelationsBefore = MakeUnique<UeAgentReflectionAuthoring::MeshMaterialRelations::FSnapshot>(*Mesh);
				for (auto& Material : Data->NewStaticMaterials)
				{
					// This compatibility contract explicitly keys slots by name.
					const int32 Source = Data->OriginalStaticMaterials.IndexOfByPredicate(
						[&](const FStaticMaterial& Item) { return Item.MaterialSlotName == Material.MaterialSlotName; });
					Data->MaterialSources.Add(Source);
					if (Source != INDEX_NONE)
					{
						const auto Interface = Material.MaterialInterface;
						Material = Data->OriginalStaticMaterials[Source];
						Material.MaterialInterface = Interface;
					}
				}
			}
			if (const FStructuredAssetChangeView* Collision =
				FindChange(Changes, TEXT("collision")))
			{
				Data->bChangeStaticCollision = true;
				if (Collision->Presence == TEXT("clear"))
				{
					Data->NewStaticGeometry = FKAggregateGeom();
					Data->NewStaticTrace = CTF_UseDefault;
				}
                else
                {
                    if (!TryRequireChange(TEXT("collision"), true, Collision->Presence, Collision->CanonicalValue, TEXT("replace"), OutError)
                        || !TryReadCollision(*Collision->CanonicalValue, Data->NewStaticGeometry, Data->NewStaticTrace, OutError)) return false;
                }
			}
			if (const FStructuredAssetChangeView* Sockets =
				FindChange(Changes, TEXT("sockets")))
			{
				Data->bChangeStaticSockets = true;
				if (!TryApplySocketPresence(
						*Sockets,
						CaptureSockets(*Mesh),
						Data->NewStaticSockets,
						OutError))
				{
					return false;
				}
			}
			if (const FStructuredAssetChangeView* BuildSettings =
				FindChange(Changes, TEXT("build_settings")))
			{
				Data->bChangeStaticBuildSettings = true;
				if (BuildSettings->Presence == TEXT("clear"))
				{
					Data->NewStaticBuildSettings.Reset();
					Data->NewStaticBuildSettings.SetNum(
						Mesh->GetNumSourceModels());
				}
                else
                {
                    if (!TryRequireChange(TEXT("build_settings"), true, BuildSettings->Presence, BuildSettings->CanonicalValue, TEXT("set"), OutError)
                        || !TryReadBuildSettings(*BuildSettings->CanonicalValue, Mesh->GetNumSourceModels(),
                            Data->OriginalStaticBuildSettings, Data->NewStaticBuildSettings, OutError)) return false;
                }
            }
			if (!Data->bChangeStaticMaterials
				&& !Data->bChangeStaticCollision
				&& !Data->bChangeStaticSockets
				&& !Data->bChangeStaticBuildSettings)
			{
                TArray<TSharedPtr<FJsonValue>> Fields;
                for (const auto& Change : Changes) Fields.Add(MakeShared<FJsonValueString>(Change.FieldId));
                return Fail(OutError, JsonFailure(TEXT("asset_data_structured_asset_changes_missing"),
                    TEXT("The update contains no supported static mesh field."), TEXT("typed_changes"),
                    MakeShared<FJsonValueArray>(Fields), MakeShared<FJsonValueString>(TEXT("materials, collision, sockets, or build_settings"))));
			}
		}
		else if (Data->Kind == EStructuredAssetKind::DataTable)
		{
			UDataTable* Table = Cast<UDataTable>(Target);
			const FStructuredAssetChangeView* RowStruct =
				FindChange(Changes, TEXT("row_struct_ref"));
			const FStructuredAssetChangeView* Rows =
				FindChange(Changes, TEXT("rows"));
            if (!Table)
                return Fail(OutError, NativeFailure(TEXT("asset_data_data_table_changes_invalid"),
                    TEXT("The update target must be a DataTable."), TEXT("target"), ObjectClassValue(Target), MakeShared<FJsonValueString>(UDataTable::StaticClass()->GetPathName())));
            if (Changes.Num() != 2)
                return Fail(OutError, JsonFailure(TEXT("asset_data_data_table_changes_invalid"),
                    TEXT("A DataTable update requires two field changes."), TEXT("typed_changes"),
                    MakeShared<FJsonValueNumber>(Changes.Num()), MakeShared<FJsonValueNumber>(2)));
            if (!TryRequireChange(TEXT("row_struct_ref"), RowStruct != nullptr, RowStruct ? RowStruct->Presence : FString(), RowStruct ? RowStruct->CanonicalValue : nullptr, TEXT("replace"), OutError)
                || !TryRequireChange(TEXT("rows"), Rows != nullptr, Rows ? Rows->Presence : FString(), Rows ? Rows->CanonicalValue : nullptr, TEXT("replace"), OutError)) return false;
            FString RowStructPath;
            if (!TryReadString(*RowStruct->CanonicalValue, RowStructPath, TEXT("row_struct_ref"), OutError)) return false;
            UScriptStruct* NewRowStruct =
				LoadObject<UScriptStruct>(
					nullptr,
					*RowStructPath);
			if (!NewRowStruct
				|| !NewRowStruct->IsChildOf(
					FTableRowBase::StaticStruct()))
			{
                return Fail(OutError, JsonFailure(TEXT("asset_data_data_table_row_struct_invalid"),
                    TEXT("The reference did not load a structure derived from TableRowBase."), TEXT("row_struct_ref"),
                    MakeShared<FJsonValueString>(RowStructPath), MakeShared<FJsonValueString>(FTableRowBase::StaticStruct()->GetPathName())));
			}
			UScriptStruct* OriginalRowStruct =
				const_cast<UScriptStruct*>(Table->GetRowStruct());
			Data->OriginalRowStruct.Reset(OriginalRowStruct);
			Data->NewRowStruct.Reset(NewRowStruct);
			Data->OriginalRows =
				CaptureRows(*Table, OriginalRowStruct);
			if (!TryReadRows(
					Resolver,
					NewRowStruct,
					*Rows->CanonicalValue,
					Data->NewRows,
					OutError))
			{
				return false;
			}
		}
		else if (Data->Kind == EStructuredAssetKind::SkeletalMesh)
		{
			USkeletalMesh* Mesh = Cast<USkeletalMesh>(Target);
			const FStructuredAssetChangeView* Materials =
				FindChange(Changes, TEXT("materials"));
			const FStructuredAssetChangeView* LodSettings =
				FindChange(Changes, TEXT("lod_settings"));
			const FStructuredAssetChangeView* Bounds =
				FindChange(Changes, TEXT("bounds"));
			const FStructuredAssetChangeView* Skeleton =
				FindChange(Changes, TEXT("skeleton_ref"));
            if (!Mesh)
                return Fail(OutError, NativeFailure(TEXT("asset_data_skeletal_mesh_changes_invalid"),
                    TEXT("The update target must be a skeletal mesh."), TEXT("target"), ObjectClassValue(Target), MakeShared<FJsonValueString>(USkeletalMesh::StaticClass()->GetPathName())));
            if (Changes.Num() != 4)
                return Fail(OutError, JsonFailure(TEXT("asset_data_skeletal_mesh_changes_invalid"),
                    TEXT("A skeletal mesh update requires four field changes."), TEXT("typed_changes"),
                    MakeShared<FJsonValueNumber>(Changes.Num()), MakeShared<FJsonValueNumber>(4)));
            if (!TryRequireChange(TEXT("materials"), Materials != nullptr, Materials ? Materials->Presence : FString(), Materials ? Materials->CanonicalValue : nullptr, TEXT("replace"), OutError)
                || !TryRequireChange(TEXT("lod_settings"), LodSettings != nullptr, LodSettings ? LodSettings->Presence : FString(), LodSettings ? LodSettings->CanonicalValue : nullptr, TEXT("replace"), OutError)
                || !TryRequireChange(TEXT("bounds"), Bounds != nullptr, Bounds ? Bounds->Presence : FString(), Bounds ? Bounds->CanonicalValue : nullptr, TEXT("replace"), OutError)
                || !TryRequireChange(TEXT("skeleton_ref"), Skeleton != nullptr, Skeleton ? Skeleton->Presence : FString(), Skeleton ? Skeleton->CanonicalValue : nullptr, TEXT("replace"), OutError)) return false;
			TArray<FMaterialSlotValue> Slots;
			if (!TryReadMaterialSlots(
					Resolver,
					*Materials->CanonicalValue,
					Slots,
					OutError))
			{
				return false;
			}
            if (!TryRequireObjectFields(*LodSettings->CanonicalValue, {TEXT("lod_settings_ref")}, TEXT("lod_settings"), OutError)) return false;
			FString LodSettingsRef;
			FString SkeletonRef;
            if (!TryReadString(*FindObjectField(*LodSettings->CanonicalValue, TEXT("lod_settings_ref")), LodSettingsRef, TEXT("lod_settings/lod_settings_ref"), OutError)
                || !TryReadString(*Skeleton->CanonicalValue, SkeletonRef, TEXT("skeleton_ref"), OutError)) return false;
            UObject* LodSettingsObject = nullptr;
			UObject* SkeletonObject = nullptr;
            if (!TryResolveStructuredReference(Resolver, LodSettingsRef, LodSettingsObject, TEXT("lod_settings/lod_settings_ref"), OutError)
                || !TryResolveStructuredReference(Resolver, SkeletonRef, SkeletonObject, TEXT("skeleton_ref"), OutError)) return false;
			USkeletalMeshLODSettings* NewLodSettings =
				Cast<USkeletalMeshLODSettings>(LodSettingsObject);
			USkeleton* NewSkeleton = Cast<USkeleton>(SkeletonObject);
            if (!NewLodSettings)
                return Fail(OutError, ReferenceClassFailure(TEXT("lod_settings/lod_settings_ref"), LodSettingsRef, LodSettingsObject, *USkeletalMeshLODSettings::StaticClass()));
            if (!NewSkeleton)
                return Fail(OutError, ReferenceClassFailure(TEXT("skeleton_ref"), SkeletonRef, SkeletonObject, *USkeleton::StaticClass()));
            if (!TryReadBounds(*Bounds->CanonicalValue, Data->NewSkeletalBounds, OutError)) return false;
			Data->OriginalSkeletalMaterials = Mesh->GetMaterials();
			Data->NewSkeletalMaterials =
				MakeSkeletalMaterials(Slots);
			Data->MaterialRelationsBefore = MakeUnique<UeAgentReflectionAuthoring::MeshMaterialRelations::FSnapshot>(*Mesh);
			for (auto& Material : Data->NewSkeletalMaterials)
			{
				const int32 Source = Data->OriginalSkeletalMaterials.IndexOfByPredicate(
					[&](const FSkeletalMaterial& Item) { return Item.MaterialSlotName == Material.MaterialSlotName; });
				Data->MaterialSources.Add(Source);
				if (Source != INDEX_NONE)
				{
					const auto Interface = Material.MaterialInterface;
					Material = Data->OriginalSkeletalMaterials[Source];
					Material.MaterialInterface = Interface;
				}
			}
			Data->OriginalLodSettings.Reset(Mesh->GetLODSettings());
			Data->NewLodSettings.Reset(NewLodSettings);
			Data->OriginalSkeletalBounds =
				Mesh->GetImportedBounds();
			Data->OriginalSkeleton.Reset(Mesh->GetSkeleton());
			Data->NewSkeleton.Reset(NewSkeleton);
		}

		OutState.Data = Data;
		return true;
	}

	bool TryApplyStructuredAssetUpdate(
		UObject* Target,
		const FStructuredAssetPreparedState& State,
		FExecutionMutationError& OutError)
	{
        ON_SCOPE_EXIT
        {
            if (!OutError.Code.IsEmpty() && Target && OutError.TargetPath.IsEmpty()) OutError.TargetPath = Target->GetPathName();
        };
        if (!Target || !State.Data.IsValid())
            return Fail(OutError, NativeFailure(TEXT("asset_data_structured_asset_state_invalid"),
                !Target ? TEXT("The apply target is no longer available.") : TEXT("The prepared structured asset state is missing."),
                !Target ? TEXT("target") : TEXT("prepared_state"), MakeShared<FJsonValueNull>(),
                MakeShared<FJsonValueString>(!Target ? TEXT("available target asset") : TEXT("prepared state"))));
		const FStructuredAssetPreparedData& Data = *State.Data;
		switch (Data.Kind)
		{
		case EStructuredAssetKind::StaticMesh:
			{
				UStaticMesh* Mesh = Cast<UStaticMesh>(Target);
				UBodySetup* BodySetup =
					Mesh ? Mesh->GetBodySetup() : nullptr;
                if (!Mesh || !BodySetup)
                    return Fail(OutError, NativeFailure(TEXT("asset_data_static_mesh_invalid"),
                        !Mesh ? TEXT("The apply target is not a static mesh.") : TEXT("The static mesh body setup is no longer available."),
                        !Mesh ? TEXT("target") : TEXT("body_setup"), !Mesh ? ObjectClassValue(Target) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()),
                        MakeShared<FJsonValueString>(!Mesh ? UStaticMesh::StaticClass()->GetPathName() : TEXT("available body setup"))));
				if (Data.bChangeStaticBuildSettings
					&& Mesh->GetNumSourceModels()
						!= Data.NewStaticBuildSettings.Num())
				{
                    return Fail(OutError, NativeFailure(TEXT("asset_data_static_mesh_source_model_changed"),
                        TEXT("The mesh source model count changed after preparation."), TEXT("build_settings/lods"),
                        MakeShared<FJsonValueNumber>(Mesh->GetNumSourceModels()), MakeShared<FJsonValueNumber>(Data.NewStaticBuildSettings.Num())));
				}
				Mesh->Modify();
				if (Data.bChangeStaticMaterials)
				{
					UeAgentReflectionAuthoring::MeshMaterialRelations::ReindexStaticSlots(
						*Mesh, Data.OriginalStaticMaterials.Num(), Data.MaterialSources);
					Mesh->SetStaticMaterials(
						Data.NewStaticMaterials);
				}
				if (Data.bChangeStaticCollision)
				{
					BodySetup->Modify();
					BodySetup->AggGeom = Data.NewStaticGeometry;
					BodySetup->CollisionTraceFlag =
						Data.NewStaticTrace;
					BodySetup->InvalidatePhysicsData();
				}
				if (Data.bChangeStaticSockets)
				{
					Mesh->Sockets.Reset();
					for (const FSocketValue& Value
						: Data.NewStaticSockets)
					{
						UStaticMeshSocket* Socket =
							NewObject<UStaticMeshSocket>(
								Mesh,
								NAME_None,
								RF_Transactional);
						Socket->SocketName = Value.Name;
						Socket->RelativeLocation =
							Value.Location;
						Socket->RelativeRotation =
							Value.Rotation;
						Socket->RelativeScale = Value.Scale;
						Mesh->Sockets.Add(Socket);
					}
				}
				if (Data.bChangeStaticBuildSettings)
				{
					for (int32 Index = 0;
						Index < Data.NewStaticBuildSettings.Num();
						++Index)
					{
						Mesh->GetSourceModel(Index).BuildSettings =
							Data.NewStaticBuildSettings[Index];
					}
				}
				Mesh->PostEditChange();
				break;
			}
		case EStructuredAssetKind::DataTable:
			{
				UDataTable* Table = Cast<UDataTable>(Target);
				if (!Table
					|| !State.Data->NewRowStruct.IsValid())
				{
                    return Fail(OutError, NativeFailure(TEXT("asset_data_data_table_apply_failed"),
                        !Table ? TEXT("The apply target is not a DataTable.") : TEXT("The prepared DataTable row structure is no longer available."),
                        !Table ? TEXT("target") : TEXT("row_struct_ref"), !Table ? ObjectClassValue(Target) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()),
                        MakeShared<FJsonValueString>(!Table ? UDataTable::StaticClass()->GetPathName() : TEXT("available prepared row structure"))));
				}
				Table->Modify();
				if (!WriteRows(
						*Table,
						State.Data->NewRowStruct.Get(),
						State.Data->NewRows, OutError))
				{
                    OutError.MutationMayHaveStarted = true;
                    return false;
				}
				Table->PostEditChange();
				break;
			}
		case EStructuredAssetKind::SkeletalMesh:
			{
				USkeletalMesh* Mesh = Cast<USkeletalMesh>(Target);
				if (!Mesh
					|| !Data.NewLodSettings.IsValid()
					|| !Data.NewSkeleton.IsValid())
				{
                    return Fail(OutError, NativeFailure(TEXT("asset_data_skeletal_mesh_invalid"),
                        !Mesh ? TEXT("The apply target is not a skeletal mesh.")
                            : !Data.NewLodSettings.IsValid() ? TEXT("The prepared LOD settings are no longer available.") : TEXT("The prepared skeleton is no longer available."),
                        !Mesh ? TEXT("target") : !Data.NewLodSettings.IsValid() ? TEXT("lod_settings/lod_settings_ref") : TEXT("skeleton_ref"),
                        !Mesh ? ObjectClassValue(Target) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()),
                        MakeShared<FJsonValueString>(!Mesh ? USkeletalMesh::StaticClass()->GetPathName()
                            : !Data.NewLodSettings.IsValid() ? USkeletalMeshLODSettings::StaticClass()->GetPathName() : USkeleton::StaticClass()->GetPathName())));
				}
				ReplaceSkeletalMeshSettings(*Mesh, Data.NewSkeletalMaterials,
					Data.NewLodSettings.Get(), Data.NewSkeletalBounds, Data.NewSkeleton.Get(),
					Data.NewLodSettings.Get() != Data.OriginalLodSettings.Get(), &Data.MaterialSources);
				break;
			}
		}
		return true;
	}

	bool TryRollbackStructuredAssetUpdate(
		UObject* Target,
		const FStructuredAssetPreparedState& State,
		FExecutionMutationError& OutError)
	{
        ON_SCOPE_EXIT { if (!OutError.IsEmpty() && Target && OutError.TargetPath.IsEmpty()) OutError.TargetPath = Target->GetPathName(); };
		if (!Target || !State.Data.IsValid())
		{
            OutError = NativeFailure(TEXT("asset_data_structured_rollback_state_invalid"),
                !Target ? TEXT("The structured asset rollback target is unavailable.") : TEXT("Structured asset rollback has no captured state."),
                !Target ? TEXT("target") : TEXT("prepared_state"),
                !Target ? ObjectClassValue(Target) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false)),
                MakeShared<FJsonValueString>(!Target ? TEXT("live structured asset") : TEXT("captured rollback state")));
            return false;
		}
		const FStructuredAssetPreparedData& Data = *State.Data;
		switch (Data.Kind)
		{
		case EStructuredAssetKind::StaticMesh:
			{
				UStaticMesh* Mesh = Cast<UStaticMesh>(Target);
				UBodySetup* BodySetup =
					Mesh ? Mesh->GetBodySetup() : nullptr;
				if (!Mesh || !BodySetup)
				{
                    OutError = NativeFailure(TEXT("asset_data_static_mesh_rollback_target_invalid"),
                        !Mesh ? TEXT("Static mesh rollback requires a StaticMesh target.") : TEXT("The static mesh has no body setup to restore."),
                        !Mesh ? TEXT("target") : TEXT("body_setup"),
                        !Mesh ? ObjectClassValue(Target) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()),
                        MakeShared<FJsonValueString>(!Mesh ? UStaticMesh::StaticClass()->GetPathName() : UBodySetup::StaticClass()->GetPathName()));
                    return false;
				}
				if (Data.bChangeStaticBuildSettings
					&& Mesh->GetNumSourceModels()
						!= Data.OriginalStaticBuildSettings.Num())
				{
                    OutError = NativeFailure(TEXT("asset_data_static_mesh_rollback_source_models_changed"),
                        TEXT("The current source model count differs from the captured build settings count."), TEXT("source_model_count"),
                        MakeShared<FJsonValueNumber>(Mesh->GetNumSourceModels()),
                        MakeShared<FJsonValueNumber>(Data.OriginalStaticBuildSettings.Num()));
                    return false;
				}
				Mesh->Modify();
				if (Data.bChangeStaticMaterials)
				{
					if (Data.MaterialRelationsBefore) Data.MaterialRelationsBefore->Restore(*Mesh);
					Mesh->SetStaticMaterials(
						Data.OriginalStaticMaterials);
				}
				if (Data.bChangeStaticCollision)
				{
					BodySetup->Modify();
					BodySetup->AggGeom =
						Data.OriginalStaticGeometry;
					BodySetup->CollisionTraceFlag =
						Data.OriginalStaticTrace;
					BodySetup->InvalidatePhysicsData();
				}
				if (Data.bChangeStaticSockets)
				{
					Mesh->Sockets = Data.OriginalStaticSockets;
				}
				if (Data.bChangeStaticBuildSettings)
				{
					for (int32 Index = 0;
						Index
							< Data.OriginalStaticBuildSettings.Num();
						++Index)
					{
						Mesh->GetSourceModel(Index).BuildSettings =
							Data.OriginalStaticBuildSettings[Index];
					}
				}
				Mesh->PostEditChange();
				break;
			}
		case EStructuredAssetKind::DataTable:
			{
				UDataTable* Table = Cast<UDataTable>(Target);
				if (!Table
					|| !State.Data->OriginalRowStruct.IsValid())
				{
                    OutError = NativeFailure(TEXT("asset_data_data_table_rollback_state_invalid"),
                        !Table ? TEXT("DataTable rollback requires a DataTable target.") : TEXT("The captured DataTable row structure is unavailable."),
                        !Table ? TEXT("target") : TEXT("row_struct"),
                        !Table ? ObjectClassValue(Target) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()),
                        MakeShared<FJsonValueString>(!Table ? UDataTable::StaticClass()->GetPathName() : TEXT("live captured row structure")));
                    return false;
				}
				Table->Modify();
				if (!WriteRows(
						*Table,
						State.Data->OriginalRowStruct.Get(),
						State.Data->OriginalRows, OutError))
				{
                    OutError.MutationMayHaveStarted = true;
                    return false;
				}
				Table->PostEditChange();
				break;
			}
		case EStructuredAssetKind::SkeletalMesh:
			{
				USkeletalMesh* Mesh = Cast<USkeletalMesh>(Target);
				if (!Mesh)
				{
                    OutError = NativeFailure(TEXT("asset_data_skeletal_mesh_rollback_target_invalid"),
                        TEXT("Skeletal mesh rollback requires a SkeletalMesh target."), TEXT("target"),
                        ObjectClassValue(Target), MakeShared<FJsonValueString>(USkeletalMesh::StaticClass()->GetPathName()));
                    return false;
				}
				ReplaceSkeletalMeshSettings(*Mesh, Data.OriginalSkeletalMaterials,
					Data.OriginalLodSettings.Get(), Data.OriginalSkeletalBounds, Data.OriginalSkeleton.Get());
				if (Data.MaterialRelationsBefore)
				{
					Data.MaterialRelationsBefore->Restore(*Mesh);
					Mesh->PostEditChange();
					FSkinnedAssetCompilingManager::Get().FinishCompilation({Mesh});
				}
				break;
			}
		}
		return true;
	}
}

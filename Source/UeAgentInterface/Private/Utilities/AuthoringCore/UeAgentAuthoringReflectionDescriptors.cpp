// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringCore/UeAgentAuthoringReflectionDescriptors.h"

#include "Utilities/UeAgentPropertyValueCodec.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace UeAgentAuthoringReflection
{
	struct FDescriptorMetadataScopeState
	{
		struct FMetadata {
			bool bIncludesPresentation = true;
			TMap<FName, FString> Values;
			uint64 Flags = 0;
			FDescriptor Fields;
			TWeakPtr<const FSharedFieldDeclaration> DeclarationSource;
		};
		TMap<const FProperty*, FMetadata> Metadata;
		int64 MetadataBytes = 0, MetadataHits = 0, MetadataSourceHandoffs = 0;
		struct FDeclaration {
			TSharedPtr<const FSharedFieldDeclaration> Data;
			TMap<FName, FString> SourceMetadata, OwnerMetadata;
			uint64 SourceFlags = 0;
			const UEnum* SourceEnum = nullptr;
			FString EnumPath;
			bool bDirectEnumMasks = false;
			struct FEnumEntry { FName Name; int64 Value; bool bHidden; };
			TArray<FEnumEntry> EnumEntries;
		};
		TMap<const FProperty*, FDeclaration> Declarations;
		int64 DeclarationCalls = 0, DeclarationHits = 0, SharedDeclarationHandoffs = 0;
		double DeclarationSeconds = 0;
		FDescriptorMetadataScopeState* Previous = nullptr;
	};
	static thread_local FDescriptorMetadataScopeState* ActiveDescriptorMetadata = nullptr;
	FScopedDescriptorMetadata::FScopedDescriptorMetadata() : State(MakeUnique<FDescriptorMetadataScopeState>())
	{
		State->Previous = ActiveDescriptorMetadata;
		ActiveDescriptorMetadata = State.Get();
	}
	FScopedDescriptorMetadata::~FScopedDescriptorMetadata()
	{
		ActiveDescriptorMetadata = State->Previous;
		if (State->MetadataHits)
			UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=reflection_metadata_scope entries=%d hits=%lld bytes=%lld validated_declaration_handoffs=%lld"),
				State->Metadata.Num(), State->MetadataHits, State->MetadataBytes, State->MetadataSourceHandoffs);
		if (State->DeclarationCalls) UE_LOG(LogTemp, Display,
			TEXT("UAI export timing phase=field_declaration_scope calls=%lld hits=%lld entries=%d total_s=%.6f shared_budget_bytes=%lld pure_declaration_handoffs=%lld"),
			State->DeclarationCalls, State->DeclarationHits, State->Declarations.Num(), State->DeclarationSeconds, State->MetadataBytes, State->SharedDeclarationHandoffs);
	}
	struct FEditDeclarationScopeState
	{
		struct FToggleRelationships {
			TMap<const FProperty*, const FProperty*> ParsedToggles;
			TMap<uint8, TMap<const FProperty*, const FProperty*>> ControllersByContext;
		};
		FEditDeclarationScopeState* Previous = nullptr;
		TMap<const UStruct*, TSharedPtr<FToggleRelationships>> Scopes;
		int32 Edges = 0;
		int64 Hits = 0, Queries = 0;
	};
	static thread_local FEditDeclarationScopeState* ActiveEditDeclarations = nullptr;
	FScopedEditDeclarations::FScopedEditDeclarations() : State(MakeUnique<FEditDeclarationScopeState>())
	{
		State->Previous = ActiveEditDeclarations;
		ActiveEditDeclarations = State.Get();
	}
	FScopedEditDeclarations::~FScopedEditDeclarations()
	{
		ActiveEditDeclarations = State->Previous;
		// Tiny per-property scopes must not emit thousands of probe log lines.
		if (State->Hits >= 4096)
			UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=edit_declaration_scope scopes=%d records=%d relation_hits=%lld toggle_queries=%lld"),
				State->Scopes.Num(), State->Edges, State->Hits, State->Queries);
	}
	namespace
	{
		bool IsPersistentField(const FProperty* Property)
		{
			return Property && !Property->HasAnyPropertyFlags(CPF_EditConst | CPF_Deprecated | CPF_Transient
				| CPF_DuplicateTransient | CPF_NonPIEDuplicateTransient | CPF_TextExportTransient);
		}

		FEditDeclarationScopeState::FToggleRelationships* GetToggleRelationships(const UStruct* Scope)
		{
			auto* State = ActiveEditDeclarations;
			if (!State) return nullptr;
			if (const auto* Found = State->Scopes.Find(Scope)) return Found->Get();
			if (State->Scopes.Num() >= 256) return nullptr;
			auto Result = MakeShared<FEditDeclarationScopeState::FToggleRelationships>();
			State->Scopes.Add(Scope, Result);
			return &Result.Get();
		}

		bool HasNativeEditDeclaration(const FProperty* Property, const UStruct* Scope,
			TFunctionRef<bool(const FProperty*)> IsDirectlyEditable, uint8 Context = 0)
		{
			if (!IsPersistentField(Property)) return false;
			if (Property->HasAnyPropertyFlags(CPF_Edit)) return IsDirectlyEditable(Property);
			if (!CastField<FBoolProperty>(Property)) return false;
			Scope = Scope ? Scope : Property->GetOwnerStruct();
			if (!Scope) return false;
			auto* Relationships = GetToggleRelationships(Scope);
			auto* Controllers = Relationships ? &Relationships->ControllersByContext.FindOrAdd(Context) : nullptr;
			if (Controllers)
				if (const auto* Controlled = Controllers->Find(Property)) {
					++ActiveEditDeclarations->Hits;
					return *Controlled && IsDirectlyEditable(*Controlled);
				}
			auto Remember = [&](const FProperty* Controlled) {
				if (Controllers && ActiveEditDeclarations->Edges < 65536) {
					Controllers->Add(Property, Controlled); ++ActiveEditDeclarations->Edges;
				}
			};
			for (TFieldIterator<FProperty> It(Scope, EFieldIterationFlags::IncludeSuper); It; ++It)
			{
				const FProperty* Controlled = *It;
				if (!IsDirectlyEditable(Controlled) || !Controlled->HasMetaData(TEXT("EditCondition"))) continue;
				const FProperty* Toggle = nullptr;
				const auto* Parsed = Relationships ? Relationships->ParsedToggles.Find(Controlled) : nullptr;
				if (Parsed) Toggle = *Parsed;
				else {
					// Preserve the original conditional read order: parsing metadata
					// can emit an engine diagnostic, so do not eagerly inspect fields
					// the current context would never reach.
					const auto& PropertyEditor = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
					Toggle = PropertyEditor.GetEditConditionToggleProperty(Controlled);
					if (ActiveEditDeclarations) ++ActiveEditDeclarations->Queries;
					if (Relationships && ActiveEditDeclarations->Edges < 65536) {
						Relationships->ParsedToggles.Add(Controlled, Toggle); ++ActiveEditDeclarations->Edges;
					}
				}
				if (Toggle == Property) { Remember(Controlled); return true; }
			}
			Remember(nullptr);
			return false;
		}

		void AddPropertyFlag(const FProperty* Property, EPropertyFlags Flag, const TCHAR* Name, TArray<FString>& OutFlags)
		{
			if (Property && Property->HasAnyPropertyFlags(Flag)) OutFlags.Add(Name);
		}

		void AddConstraint(const FProperty* Property, const TCHAR* SourceKey, const TCHAR* OutputKey, TMap<FString, FString>& OutConstraints)
		{
			if (!Property) return;
			const FString Value = Property->GetMetaData(SourceKey);
			if (!Value.IsEmpty()) OutConstraints.Add(OutputKey, Value);
		}

		TArray<TSharedPtr<FJsonValue>> MakeStringArray(const TArray<FString>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> Result;
			Result.Reserve(Values.Num());
			for (const FString& Value : Values) Result.Add(MakeShared<FJsonValueString>(Value));
			return Result;
		}
	}

	TSharedPtr<FJsonObject> FDescriptor::MakeAvailabilityJson() const
	{
		if (bEditable && EditCondition.IsEmpty() && !bEditConditionHides) return nullptr;
		TSharedPtr<FJsonObject> Availability = MakeShared<FJsonObject>();
		Availability->SetBoolField(TEXT("enabled"), bEditable);
		Availability->SetBoolField(TEXT("editable"), bEditable);
		if (!EditCondition.IsEmpty()) Availability->SetStringField(TEXT("edit_condition"), EditCondition);
		if (bEditConditionHides) Availability->SetBoolField(TEXT("edit_condition_hides"), true);
		if (!bEditable && !AvailabilityReason.IsEmpty()) Availability->SetStringField(TEXT("reason"), AvailabilityReason);
		return Availability;
	}

	TSharedPtr<FJsonObject> FDescriptor::MakeConstraintsJson() const
	{
		if (GetConstraints().IsEmpty()) return nullptr;
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		for (const TPair<FString, FString>& Pair : GetConstraints()) Result->SetStringField(Pair.Key, Pair.Value);
		return Result;
	}

	TArray<TSharedPtr<FJsonValue>> FDescriptor::MakeCategoryPathJson() const
	{
		return MakeStringArray(GetCategoryPath());
	}

	TArray<TSharedPtr<FJsonValue>> FDescriptor::MakePropertyFlagsJson() const
	{
		return MakeStringArray(PropertyFlags);
	}

	void ReadConstraints(const FProperty* Property, TMap<FString, FString>& OutConstraints)
	{
		OutConstraints.Reset();
		AddConstraint(Property, TEXT("ClampMin"), TEXT("clamp_min"), OutConstraints);
		AddConstraint(Property, TEXT("ClampMax"), TEXT("clamp_max"), OutConstraints);
		AddConstraint(Property, TEXT("UIMin"), TEXT("ui_min"), OutConstraints);
		AddConstraint(Property, TEXT("UIMax"), TEXT("ui_max"), OutConstraints);
		AddConstraint(Property, TEXT("Units"), TEXT("units"), OutConstraints);
		AddConstraint(Property, TEXT("ForceUnits"), TEXT("force_units"), OutConstraints);
	}

	static bool SameDeclarationType(const UeAgentAuthoringTypeDescriptors::FDescriptor& A,
		const UeAgentAuthoringTypeDescriptors::FDescriptor& B) {
		// Specialized meaning may be a dynamic tree. It keeps the original path;
		// never use FDescriptor's native/FName-like comparison for exact JSON text.
		if (A.ValueSemantics || B.ValueSemantics || A.FixedLength != B.FixedLength
			|| A.bRequiresHashSource != B.bRequiresHashSource
			|| !A.Kind.Equals(B.Kind, ESearchCase::CaseSensitive)
			|| !A.StructPath.Equals(B.StructPath, ESearchCase::CaseSensitive)
			|| !A.EnumPath.Equals(B.EnumPath, ESearchCase::CaseSensitive)
			|| !A.AllowedClassPath.Equals(B.AllowedClassPath, ESearchCase::CaseSensitive)) return false;
		const auto Child = [](const auto& X, const auto& Y) {
			return X.IsValid() == Y.IsValid() && (!X || SameDeclarationType(*X, *Y));
		};
		return Child(A.ElementType, B.ElementType) && Child(A.KeyType, B.KeyType)
			&& Child(A.MappedType, B.MappedType) && Child(A.InnerType, B.InnerType);
	}

	static int64 DeclarationTypeBytes(const UeAgentAuthoringTypeDescriptors::FDescriptor& Type) {
		int64 Bytes = sizeof(Type) + 64 + Type.Kind.GetAllocatedSize() + Type.StructPath.GetAllocatedSize()
			+ Type.EnumPath.GetAllocatedSize() + Type.AllowedClassPath.GetAllocatedSize();
		for (const auto* Child : {&Type.ElementType, &Type.KeyType, &Type.MappedType, &Type.InnerType})
			if (*Child) Bytes += DeclarationTypeBytes(**Child);
		return Bytes;
	}

	static int64 DeclarationJsonBytes(const TSharedPtr<FJsonValue>& Value) {
		if (!Value) return 0;
		// Conservative node/control-block allowance plus all owned containers/text.
		int64 Bytes = 128;
		if (Value->Type == EJson::String) Bytes += Value->AsString().GetAllocatedSize();
		else if (Value->Type == EJson::Array) {
			Bytes += Value->AsArray().GetAllocatedSize();
			for (const auto& Item : Value->AsArray()) Bytes += DeclarationJsonBytes(Item);
		} else if (Value->Type == EJson::Object) {
			Bytes += sizeof(FJsonObject) + Value->AsObject()->Values.GetAllocatedSize();
			for (const auto& Item : Value->AsObject()->Values)
				Bytes += Item.Key.GetAllocatedSize() + DeclarationJsonBytes(Item.Value);
		}
		return Bytes;
	}

	static TSharedPtr<const FSharedFieldDeclaration> CaptureFieldDeclarationData(const FProperty* Property) {
		const double Started = ActiveDescriptorMetadata ? FPlatformTime::Seconds() : 0;
		ON_SCOPE_EXIT { if (ActiveDescriptorMetadata) ActiveDescriptorMetadata->DeclarationSeconds += FPlatformTime::Seconds() - Started; };
		const auto SameMetadata = [](const FProperty* Field, const TMap<FName, FString>& Before) {
			const auto* Now = Field ? Field->GetMetaDataMap() : nullptr;
			if (Before.Num() != (Now ? Now->Num() : 0)) return false;
			if (Now) for (const auto& Item : *Now) {
				const auto* Previous = Before.Find(Item.Key);
				if (!Previous || !Previous->Equals(Item.Value, ESearchCase::CaseSensitive)) return false;
			}
			return true;
		};
		const FProperty* Owner = Property ? Property->GetOwnerProperty() : nullptr;
		const auto* Enum = UeAgentAuthoringTypeDescriptors::GetPropertyEnum(Property);
		auto* Cached = ActiveDescriptorMetadata ? ActiveDescriptorMetadata->Declarations.Find(Property) : nullptr;
		if (ActiveDescriptorMetadata) {
			++ActiveDescriptorMetadata->DeclarationCalls;
			bool bSame = Cached && Cached->SourceFlags == (Property ? uint64(Property->GetPropertyFlags()) : 0)
				&& SameMetadata(Property, Cached->SourceMetadata) && (Owner == Property || SameMetadata(Owner, Cached->OwnerMetadata))
				&& Enum == Cached->SourceEnum;
			if (bSame && Enum) {
				bSame = Enum->NumEnums() == Cached->EnumEntries.Num()
					&& Enum->GetPathName().Equals(Cached->EnumPath, ESearchCase::CaseSensitive)
					&& Enum->GetBoolMetaData(TEXT("UseEnumValuesAsMaskValuesInEditor")) == Cached->bDirectEnumMasks;
				for (int32 Index = 0; bSame && Index < Enum->NumEnums(); ++Index) {
					const auto& Before = Cached->EnumEntries[Index];
					bSame = Enum->GetNameByIndex(Index).GetDisplayIndex() == Before.Name.GetDisplayIndex()
						&& Enum->GetValueByIndex(Index) == Before.Value
						&& Enum->HasMetaData(TEXT("Hidden"), Index) == Before.bHidden;
				}
			}
			if (bSame) {
				// Compare real declaration sources, not freshly rebuilt type,
				// constraints and candidate JSON. Dynamic schema trees are not admitted.
				++ActiveDescriptorMetadata->DeclarationHits;
				return Cached->Data;
			}
		}
		FDescriptorMetadataScopeState::FDeclaration Current;
		const auto Data = MakeShared<FSharedFieldDeclaration>(); Current.Data = Data;
		Data->Type = UeAgentAuthoringTypeDescriptors::Describe(Property);
		ReadConstraints(Property, Data->Constraints);
		Data->Fragment = MakeShared<FJsonObject>();
		Data->Fragment->SetObjectField(TEXT("type"), Data->Type.ToJsonObject());
		if (!Data->Constraints.IsEmpty()) {
			const auto Constraints = MakeShared<FJsonObject>();
			for (const auto& Item : Data->Constraints) Constraints->SetStringField(Item.Key, Item.Value);
			Data->Fragment->SetObjectField(TEXT("constraints"), Constraints);
		}
		if (const auto Candidates = UeAgentAuthoringTypeDescriptors::BuildEnumCandidates(Enum, false, Property))
			for (const auto& Item : Candidates->Values) Data->Fragment->SetField(Item.Key, Item.Value);
		if (ActiveDescriptorMetadata && !Cached && ActiveDescriptorMetadata->Declarations.Num() < 2048
			&& SameDeclarationType(Data->Type, Data->Type)) {
			Current.SourceFlags = Property ? uint64(Property->GetPropertyFlags()) : 0;
			if (Property) if (const auto* Map = Property->GetMetaDataMap()) Current.SourceMetadata = *Map;
			if (Owner && Owner != Property) if (const auto* Map = Owner->GetMetaDataMap()) Current.OwnerMetadata = *Map;
			Current.SourceEnum = Enum;
			if (Enum) {
				Current.EnumPath = Enum->GetPathName();
				Current.bDirectEnumMasks = Enum->GetBoolMetaData(TEXT("UseEnumValuesAsMaskValuesInEditor"));
				for (int32 Index = 0; Index < Enum->NumEnums(); ++Index)
					Current.EnumEntries.Add({Enum->GetNameByIndex(Index), Enum->GetValueByIndex(Index), Enum->HasMetaData(TEXT("Hidden"), Index)});
			}
			int64 Bytes = sizeof(Current) + sizeof(*Data) + 160 + DeclarationTypeBytes(Data->Type)
				+ Data->Constraints.GetAllocatedSize()
				+ Current.SourceMetadata.GetAllocatedSize() + Current.OwnerMetadata.GetAllocatedSize()
				+ Current.EnumEntries.GetAllocatedSize() + Current.EnumPath.GetAllocatedSize()
				+ DeclarationJsonBytes(MakeShared<FJsonValueObject>(Data->Fragment));
			for (const auto* Map : {&Current.SourceMetadata, &Current.OwnerMetadata})
				for (const auto& Item : *Map) Bytes += Item.Value.GetAllocatedSize();
			for (const auto& Item : Data->Constraints) Bytes += Item.Key.GetAllocatedSize() + Item.Value.GetAllocatedSize();
			if (ActiveDescriptorMetadata->MetadataBytes + Bytes <= 8 * 1024 * 1024) {
				ActiveDescriptorMetadata->MetadataBytes += Bytes;
				ActiveDescriptorMetadata->Declarations.Add(Property, Current);
			}
		}
		return Data;
	}
	static TSharedPtr<FJsonObject> CaptureFieldDeclarationImpl(const FProperty* Property,
		UeAgentAuthoringTypeDescriptors::FDescriptor* OutType, TMap<FString, FString>* OutConstraints) {
		const auto Data = CaptureFieldDeclarationData(Property);
		if (OutType) *OutType = Data->Type;
		if (OutConstraints) *OutConstraints = Data->Constraints;
		return Data->Fragment;
	}
	TSharedPtr<FJsonObject> CaptureFieldDeclaration(const FProperty* Property) {
		return CaptureFieldDeclarationImpl(Property, nullptr, nullptr);
	}
	TSharedPtr<FJsonObject> CaptureFieldDeclaration(const FProperty* Property,
		UeAgentAuthoringTypeDescriptors::FDescriptor& OutType, TMap<FString, FString>& OutConstraints) {
		return CaptureFieldDeclarationImpl(Property, &OutType, &OutConstraints);
	}

	bool IsEditableDeclaration(const FProperty* Property, const UStruct* Scope)
	{
		Property = Property ? Property->GetOwnerProperty() : nullptr;
		return HasNativeEditDeclaration(Property, Scope, [](const FProperty* Candidate)
		{
			return IsPersistentField(Candidate) && Candidate->HasAnyPropertyFlags(CPF_Edit);
		});
	}

	bool IsEditableByFlags(const FProperty* Property, const UObject* Owner, bool bShowDefaultsOnly, bool bUseInstanceEditability, const UStruct* Scope)
	{
		// Container elements inherit their declaration; toggle controls inherit the native editing context.
		Property = Property ? Property->GetOwnerProperty() : nullptr;
		if (!Property) return false;
		if (!Scope)
		{
			Scope = Property->GetOwnerStruct();
			const UClass* DeclaringClass = Cast<UClass>(Scope);
			if (Owner && DeclaringClass && Owner->IsA(DeclaringClass)) Scope = Owner->GetClass();
		}
		const bool bTemplateOwner = !bUseInstanceEditability && Owner && Owner->IsTemplate();
		auto IsAvailableInContext = [&](const FProperty* Candidate)
		{
			if (!bShowDefaultsOnly && Candidate->HasAnyPropertyFlags(CPF_DisableEditOnInstance) && (!Owner || !bTemplateOwner)) return false;
			return !Candidate->HasAnyPropertyFlags(CPF_DisableEditOnTemplate) || (Owner && !bTemplateOwner);
		};
		return IsAvailableInContext(Property) && HasNativeEditDeclaration(Property, Scope, [&](const FProperty* Candidate)
		{
			return IsPersistentField(Candidate) && Candidate->HasAnyPropertyFlags(CPF_Edit) && IsAvailableInContext(Candidate);
		}, uint8(8 | (bShowDefaultsOnly ? 1 : 0) | (Owner ? 2 : 0) | (bTemplateOwner ? 4 : 0)));
	}

	bool IsOwnedObjectProperty(const FProperty* Property)
	{
		const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property);
		if (!ObjectProperty || CastField<FClassProperty>(Property)) return false;
		return Property->HasAnyPropertyFlags(CPF_InstancedReference)
			|| (ObjectProperty->PropertyClass
				&& ObjectProperty->PropertyClass->HasAnyClassFlags(CLASS_EditInlineNew)
				&& Property->HasAnyPropertyFlags(CPF_ExportObject));
	}

	bool IsExistingInlineObjectProperty(
		const FProperty* Property,
		const void* ValuePtr,
		const UObject* Owner)
	{
		const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property);
		if (!ObjectProperty || CastField<FClassProperty>(Property) || !ValuePtr || !Owner
			|| !Property->HasAnyPropertyFlags(CPF_ExportObject)
			|| !Property->HasMetaData(TEXT("EditInline"))
			|| Property->HasMetaData(TEXT("NoEditInline")))
		{
			return false;
		}
		const UObject* Referenced = ObjectProperty->GetObjectPropertyValue(ValuePtr);
		return Referenced && Referenced->IsIn(Owner);
	}

	bool IsEditableProperty(const FProperty* Property, UObject* Owner, FString* OutReason, bool bShowDefaultsOnly, bool bUseInstanceEditability, const UStruct* Scope)
	{
		if (OutReason) OutReason->Reset();
		if (!IsEditableByFlags(Property, Owner, bShowDefaultsOnly, bUseInstanceEditability, Scope))
		{
			if (OutReason) *OutReason = TEXT("not_editable_by_property_flags");
			return false;
		}
		return true;
	}

	static bool BuildImpl(const FProperty* Property, UObject* Owner, const void* ValuePtr, FDescriptor& OutDescriptor, FDiagnostic& OutError,
		bool bShowDefaultsOnly, bool bUseInstanceEditability, const UStruct* Scope, bool bPresentation, bool bShareDeclaration = false)
	{
		OutDescriptor = FDescriptor();
		if (!Property)
		{
			OutError = FDiagnostic::Failure(TEXT("invalid_property"),
				TEXT("A reflected property descriptor cannot be built without a native property."), TEXT("property_descriptor"));
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = TEXT("property");
			OutError.ExpectedValuePresent = true;
			OutError.ActualValuePresent = false;
			if (Owner) OutError.InternalContext = Owner->GetPathName();
			return false;
		}

		if (bPresentation) {
			OutDescriptor.PropertyName = Property->GetName();
			OutDescriptor.OwnerStructPath = Property->GetOwnerStruct() ? Property->GetOwnerStruct()->GetPathName() : FString();
			OutDescriptor.DisplayName = Property->GetDisplayNameText().ToString();
			OutDescriptor.Tooltip = Property->GetToolTipText().ToString();
		} else if (bShareDeclaration) {
			OutDescriptor.SharedDeclaration = CaptureFieldDeclarationData(Property);
			OutDescriptor.FieldDeclaration = OutDescriptor.SharedDeclaration->Fragment;
			if (ActiveDescriptorMetadata) ++ActiveDescriptorMetadata->SharedDeclarationHandoffs;
		} else {
			OutDescriptor.FieldDeclaration = CaptureFieldDeclaration(Property, OutDescriptor.Type, OutDescriptor.Constraints);
		}
		if (!bShareDeclaration) {
			OutDescriptor.bEditorOnly = Property->IsEditorOnlyProperty();
			OutDescriptor.bTransient = Property->HasAnyPropertyFlags(CPF_Transient);
			OutDescriptor.bDeprecated = Property->HasAnyPropertyFlags(CPF_Deprecated) || Property->HasMetaData(TEXT("DeprecatedProperty"));
		}
		// Only these fields read the property's own metadata/flags. Validate the
		// actual map on every call, including case-only changes by a callback.
		// Type, display text, owner and all instance/codec decisions stay current.
		const auto* Metadata = Property->GetMetaDataMap();
		auto* Cached = ActiveDescriptorMetadata ? ActiveDescriptorMetadata->Metadata.Find(Property) : nullptr;
		// CaptureFieldDeclarationData just checked these actual flags and the
		// complete metadata map. Same immutable source => same category facts;
		// no callback runs between that check and this synchronous consumer.
		const bool bSameDeclaredSource = bShareDeclaration && Cached
			&& Cached->DeclarationSource.Pin() == OutDescriptor.SharedDeclaration;
		bool bSameMetadata = Cached && (!bPresentation || Cached->bIncludesPresentation)
			&& (bSameDeclaredSource || (Cached->Flags == uint64(Property->GetPropertyFlags())
				&& Cached->Values.Num() == (Metadata ? Metadata->Num() : 0)));
		if (bSameMetadata && !bSameDeclaredSource && Metadata) for (const auto& Item : *Metadata) {
			const auto* Before = Cached->Values.Find(Item.Key);
			if (!Before || !Before->Equals(Item.Value, ESearchCase::CaseSensitive)) { bSameMetadata = false; break; }
		}
		if (bSameMetadata) {
			const auto& Type = Cached->Fields.Type;
			const bool bStaticScalar = !Type.ValueSemantics && !Type.ElementType && !Type.KeyType && !Type.MappedType && !Type.InnerType
				&& Type.FixedLength == INDEX_NONE && Type.EnumPath.IsEmpty() && Type.StructPath.IsEmpty() && Type.AllowedClassPath.IsEmpty()
				&& !UeAgentAuthoringTypeDescriptors::GetPropertyEnum(Property);
			if (bPresentation) OutDescriptor.Type = bStaticScalar
				? Cached->Fields.Type : UeAgentAuthoringTypeDescriptors::Describe(Property);
			if (bShareDeclaration) {
				OutDescriptor.SharedCategoryPath = Cached->Fields.SharedCategoryPath;
				Cached->DeclarationSource = OutDescriptor.SharedDeclaration;
				if (bSameDeclaredSource) ++ActiveDescriptorMetadata->MetadataSourceHandoffs;
			} else {
				OutDescriptor.EditCondition = Cached->Fields.EditCondition;
				OutDescriptor.bEditConditionHides = Cached->Fields.bEditConditionHides;
				OutDescriptor.CategoryPath = Cached->Fields.GetCategoryPath();
			}
			if (bPresentation) {
				OutDescriptor.PropertyFlags = Cached->Fields.PropertyFlags;
				OutDescriptor.Constraints = Cached->Fields.Constraints;
			}
			++ActiveDescriptorMetadata->MetadataHits;
		} else {
		if (bPresentation) OutDescriptor.Type = UeAgentAuthoringTypeDescriptors::Describe(Property);
		OutDescriptor.EditCondition = Property->GetMetaData(TEXT("EditCondition"));
		OutDescriptor.bEditConditionHides = Property->HasMetaData(TEXT("EditConditionHides"));

		TArray<FString> CategoryParts;
		Property->GetMetaData(TEXT("Category")).ParseIntoArray(CategoryParts, TEXT("|"), true);
		for (FString Part : CategoryParts)
		{
			Part.TrimStartAndEndInline();
			if (!Part.IsEmpty()) OutDescriptor.CategoryPath.Add(MoveTemp(Part));
		}
		if (bShareDeclaration) OutDescriptor.SharedCategoryPath = MakeShared<const TArray<FString>>(MoveTemp(OutDescriptor.CategoryPath));

		if (bPresentation) {
		AddPropertyFlag(Property, CPF_Edit, TEXT("Edit"), OutDescriptor.PropertyFlags);
		AddPropertyFlag(Property, CPF_EditConst, TEXT("EditConst"), OutDescriptor.PropertyFlags);
		AddPropertyFlag(Property, CPF_BlueprintVisible, TEXT("BlueprintVisible"), OutDescriptor.PropertyFlags);
		AddPropertyFlag(Property, CPF_BlueprintReadOnly, TEXT("BlueprintReadOnly"), OutDescriptor.PropertyFlags);
		AddPropertyFlag(Property, CPF_Config, TEXT("Config"), OutDescriptor.PropertyFlags);
		AddPropertyFlag(Property, CPF_GlobalConfig, TEXT("GlobalConfig"), OutDescriptor.PropertyFlags);
		AddPropertyFlag(Property, CPF_Transient, TEXT("Transient"), OutDescriptor.PropertyFlags);
		AddPropertyFlag(Property, CPF_Deprecated, TEXT("Deprecated"), OutDescriptor.PropertyFlags);
		AddPropertyFlag(Property, CPF_DuplicateTransient, TEXT("DuplicateTransient"), OutDescriptor.PropertyFlags);
		AddPropertyFlag(Property, CPF_NonPIEDuplicateTransient, TEXT("NonPIEDuplicateTransient"), OutDescriptor.PropertyFlags);
		AddPropertyFlag(Property, CPF_TextExportTransient, TEXT("TextExportTransient"), OutDescriptor.PropertyFlags);
		AddPropertyFlag(Property, CPF_SaveGame, TEXT("SaveGame"), OutDescriptor.PropertyFlags);
		}

		if (bPresentation) ReadConstraints(Property, OutDescriptor.Constraints);
		if (ActiveDescriptorMetadata && !Cached && ActiveDescriptorMetadata->Metadata.Num() < 4096) {
			FDescriptorMetadataScopeState::FMetadata Candidate;
			Candidate.bIncludesPresentation = bPresentation;
			if (Metadata) Candidate.Values = *Metadata;
			Candidate.Flags = uint64(Property->GetPropertyFlags());
			Candidate.Fields.Type = OutDescriptor.GetType();
			Candidate.Fields.EditCondition = OutDescriptor.EditCondition;
			Candidate.Fields.bEditConditionHides = OutDescriptor.bEditConditionHides;
			Candidate.Fields.SharedCategoryPath = OutDescriptor.SharedCategoryPath ? OutDescriptor.SharedCategoryPath
				: MakeShared<const TArray<FString>>(OutDescriptor.CategoryPath);
			Candidate.DeclarationSource = OutDescriptor.SharedDeclaration;
			Candidate.Fields.PropertyFlags = OutDescriptor.PropertyFlags;
			Candidate.Fields.Constraints = OutDescriptor.Constraints;
			int64 Bytes = sizeof(Candidate) + 128 + DeclarationTypeBytes(Candidate.Fields.Type) + Candidate.Values.GetAllocatedSize()
				+ Candidate.Fields.EditCondition.GetAllocatedSize() + Candidate.Fields.GetCategoryPath().GetAllocatedSize() + 64
				+ Candidate.Fields.PropertyFlags.GetAllocatedSize() + Candidate.Fields.Constraints.GetAllocatedSize();
			for (const auto& Item : Candidate.Values) Bytes += Item.Value.GetAllocatedSize();
			for (const auto& Item : Candidate.Fields.GetCategoryPath()) Bytes += Item.GetAllocatedSize();
			for (const auto& Item : Candidate.Fields.PropertyFlags) Bytes += Item.GetAllocatedSize();
			for (const auto& Item : Candidate.Fields.Constraints) Bytes += Item.Key.GetAllocatedSize() + Item.Value.GetAllocatedSize();
			if (ActiveDescriptorMetadata->MetadataBytes + Bytes <= 8 * 1024 * 1024) {
				ActiveDescriptorMetadata->MetadataBytes += Bytes;
				ActiveDescriptorMetadata->Metadata.Add(Property, MoveTemp(Candidate));
			}
		}
		}

		if (bShareDeclaration && !OutDescriptor.SharedCategoryPath) {
			OutError = FDiagnostic::Failure(TEXT("property_declaration_category_handoff_missing"),
				TEXT("Stage=property_descriptor; the selected authoring declaration producer did not supply its category array. Producer=BuildAuthoringSharedDeclaration; no alternate descriptor was selected."), TEXT("property_descriptor"));
			OutError.PropertyPath = TEXT("category_path"); return false;
		}
		FString PropertyReason;
		OutDescriptor.bEditableByProperty = IsEditableProperty(Property, Owner, &PropertyReason, bShowDefaultsOnly, bUseInstanceEditability, Scope)
			|| IsExistingInlineObjectProperty(Property, ValuePtr, Owner);
		FDiagnostic CodecReason;
		OutDescriptor.bCodecSupported = UeAgentPropertyValueCodec::IsAuthoringSupported(Property, ValuePtr, CodecReason);
		OutDescriptor.bEditable = OutDescriptor.bEditableByProperty && OutDescriptor.bCodecSupported;
		if (!OutDescriptor.bEditable)
		{
			OutDescriptor.AvailabilityReason = OutDescriptor.bCodecSupported ? PropertyReason : CodecReason.Code;
		}

		OutError.Reset();
		return true;
	}
	bool Build(const FProperty* Property, UObject* Owner, const void* ValuePtr, FDescriptor& OutDescriptor, FDiagnostic& OutError,
		bool bShowDefaultsOnly, bool bUseInstanceEditability, const UStruct* Scope)
	{
		return BuildImpl(Property, Owner, ValuePtr, OutDescriptor, OutError, bShowDefaultsOnly, bUseInstanceEditability, Scope, true);
	}
	bool BuildAuthoring(const FProperty* Property, UObject* Owner, const void* ValuePtr, FDescriptor& OutDescriptor, FDiagnostic& OutError,
		bool bShowDefaultsOnly, bool bUseInstanceEditability, const UStruct* Scope)
	{
		return BuildImpl(Property, Owner, ValuePtr, OutDescriptor, OutError, bShowDefaultsOnly, bUseInstanceEditability, Scope, false);
	}
	bool BuildAuthoringSharedDeclaration(const FProperty* Property, UObject* Owner, const void* ValuePtr, FDescriptor& OutDescriptor, FDiagnostic& OutError,
		bool bShowDefaultsOnly, bool bUseInstanceEditability, const UStruct* Scope)
	{
		return BuildImpl(Property, Owner, ValuePtr, OutDescriptor, OutError, bShowDefaultsOnly, bUseInstanceEditability, Scope, false, true);
	}
}

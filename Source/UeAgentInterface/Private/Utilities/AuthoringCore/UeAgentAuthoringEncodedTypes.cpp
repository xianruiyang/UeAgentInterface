// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringCore/UeAgentAuthoringEncodedTypes.h"

#include "Utilities/AuthoringCore/UeAgentAuthoringTypeDescriptors.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UeAgentAuthoringEncodedTypes
{
	struct FScope::FImpl
	{
		struct FCaptured
		{
			// Keep encoded values alive: pointer reuse must never assign another value's type.
			TSharedPtr<FJsonValue> Value;
			TSharedPtr<FJsonObject> Descriptor;
		};
		static thread_local FImpl* Active;
		FImpl* Previous = nullptr;
		TMap<const FJsonValue*, FCaptured> Values;
		TMap<const FJsonObject*, const FJsonValue*> Objects;
		struct FOmittedFields
		{
			TSharedPtr<FJsonObject> Parent;
			TMap<FString, TSharedPtr<FJsonValue>> Values;
		};
		TMap<const FJsonObject*, FOmittedFields> OmittedFields;
		struct FFieldEmissions
		{
			TSharedPtr<FJsonObject> Parent;
			TMap<FString, EFieldEmission> Fields;
		};
		TMap<const FJsonObject*, FFieldEmissions> FieldEmissions;

		const FFieldEmissions* FindEmissions(const FJsonObject* Object) const
		{
			const auto* Result = FieldEmissions.Find(Object);
			return Result ? Result : Previous ? Previous->FindEmissions(Object) : nullptr;
		}

		bool HasEmissions() const
		{
			return !FieldEmissions.IsEmpty() || (Previous && Previous->HasEmissions());
		}

		const FOmittedFields* FindOmitted(const FJsonObject* Object) const
		{
			const auto* Result = OmittedFields.Find(Object);
			return Result ? Result : Previous ? Previous->FindOmitted(Object) : nullptr;
		}

		const FCaptured* Find(const TSharedPtr<FJsonValue>& Value) const
		{
			if (!Value) return nullptr;
			const FCaptured* Captured = Values.Find(Value.Get());
			if (!Captured && Value->Type == EJson::Object)
			{
				if (const auto* Key = Objects.Find(Value->AsObject().Get())) Captured = Values.Find(*Key);
			}
			return Captured ? Captured : Previous ? Previous->Find(Value) : nullptr;
		}

		TSharedPtr<FJsonObject> Describe(const TSharedPtr<FJsonValue>& Value) const
		{
			if (!Value) return nullptr;
			const FCaptured* Captured = Find(Value);
			const auto* Omitted = Value->Type == EJson::Object ? FindOmitted(Value->AsObject().Get()) : nullptr;

			auto Definition = MakeShared<FJsonObject>();
			auto Type = MakeShared<FJsonObject>();
			if (Captured)
			{
				const TSharedPtr<FJsonObject>* Existing = nullptr;
				if (Captured->Descriptor->TryGetObjectField(TEXT("type"), Existing)) Type->Values = (*Existing)->Values;
				if (Captured->Descriptor->TryGetObjectField(TEXT("value_definition"), Existing)) Definition->Values = (*Existing)->Values;
			}
			if (Value->Type == EJson::Object)
			{
				auto Fields = MakeShared<FJsonObject>();
				const TSharedPtr<FJsonObject>* ExistingFields = nullptr;
				if (Definition->TryGetObjectField(TEXT("fields"), ExistingFields)) Fields->Values = (*ExistingFields)->Values;
				if (Omitted)
					for (const auto& Field : Omitted->Values)
					{
						if (Value->AsObject()->HasField(Field.Key)) continue;
						const auto NativeDescriptor = Describe(Field.Value);
						const auto Descriptor = MakeShared<FJsonObject>();
						if (NativeDescriptor) Descriptor->Values = NativeDescriptor->Values;
						else if (const auto Existing = Fields->TryGetField(Field.Key); Existing && Existing->Type == EJson::Object)
							Descriptor->Values = Existing->AsObject()->Values;
						Descriptor->SetField(TEXT("omitted_value"), Field.Value);
						Fields->SetObjectField(Field.Key, Descriptor);
					}
				for (const auto& Field : Value->AsObject()->Values)
				{
					if (auto Descriptor = Describe(Field.Value)) Fields->SetObjectField(Field.Key, Descriptor);
				}
				if (Fields->Values.IsEmpty()) return Captured ? Captured->Descriptor : nullptr;
				if (!Captured) Type->SetStringField(TEXT("kind"), TEXT("object"));
				Definition->SetObjectField(TEXT("fields"), Fields);
			}
			else if (Value->Type == EJson::Array)
			{
				TArray<TSharedPtr<FJsonValue>> Items;
				bool bHasTypes = false;
				for (const auto& Item : Value->AsArray())
				{
					const auto Descriptor = Describe(Item);
					bHasTypes |= Descriptor.IsValid();
					Items.Add(Descriptor ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueObject>(Descriptor))
						: TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()));
				}
				if (!bHasTypes) return Captured ? Captured->Descriptor : nullptr;
				if (!Captured) Type->SetStringField(TEXT("kind"), TEXT("array"));
				Definition->SetArrayField(TEXT("items"), Items);
			}
			else return Captured ? Captured->Descriptor : nullptr;
			auto Descriptor = MakeShared<FJsonObject>();
			Descriptor->SetObjectField(TEXT("type"), Type);
			Descriptor->SetObjectField(TEXT("value_definition"), Definition);
			return Descriptor;
		}
	};

	thread_local FScope::FImpl* FScope::FImpl::Active = nullptr;

	FScope::FScope() : Impl(MakeUnique<FImpl>())
	{
		Impl->Previous = FImpl::Active;
		FImpl::Active = Impl.Get();
	}

	FScope::~FScope()
	{
		// A nested capture may return values to the enclosing document. Preserve
		// their declared types along with their shared ownership, not just pointers.
		if (Impl->Previous)
		{
			for (const auto& Item : Impl->Values) Impl->Previous->Values.Add(Item.Key, Item.Value);
			for (const auto& Item : Impl->Objects) Impl->Previous->Objects.Add(Item.Key, Item.Value);
			for (const auto& Item : Impl->OmittedFields)
			{
				auto& Parent = Impl->Previous->OmittedFields.FindOrAdd(Item.Key);
				Parent.Parent = Item.Value.Parent;
				for (const auto& Field : Item.Value.Values) Parent.Values.Add(Field.Key, Field.Value);
			}
			for (const auto& Item : Impl->FieldEmissions)
			{
				auto& Parent = Impl->Previous->FieldEmissions.FindOrAdd(Item.Key);
				Parent.Parent = Item.Value.Parent;
				for (const auto& Field : Item.Value.Fields) Parent.Fields.Add(Field.Key, Field.Value);
			}
		}
		FImpl::Active = Impl->Previous;
	}

	bool FScope::IsActive() { return FImpl::Active != nullptr; }

	void FScope::Record(const TSharedPtr<FJsonValue>& Value,
		const UeAgentAuthoringTypeDescriptors::FDescriptor& Type, const TSharedPtr<FJsonObject>& Definition)
	{
		if (!FImpl::Active || !Value || Value->Type == EJson::Null) return;
		auto Descriptor = MakeShared<FJsonObject>();
		auto NativeType = Type.ToJsonObject();
		const TSharedPtr<FJsonObject>* DeclaredType = nullptr;
		if (Definition && Definition->TryGetObjectField(TEXT("type"), DeclaredType))
			NativeType->Values = (*DeclaredType)->Values;
		if (const auto* Existing = FImpl::Active->Find(Value))
		{
			const auto PreviousType = Existing->Descriptor->GetObjectField(TEXT("type"));
			if (PreviousType->GetStringField(TEXT("kind")) == Type.Kind)
			{
				for (const auto& Field : PreviousType->Values)
					if (!NativeType->HasField(Field.Key)) NativeType->SetField(Field.Key, Field.Value);
				Descriptor->Values = Existing->Descriptor->Values;
			}
		}
		Descriptor->SetObjectField(TEXT("type"), NativeType);
		if (Definition)
		{
			const auto Body = MakeShared<FJsonObject>(); Body->Values = Definition->Values;
			// Only an object-valued type is a captured descriptor. A string-valued
			// type belongs to a public value-definition contract and must survive.
			if (DeclaredType) Body->RemoveField(TEXT("type"));
			Descriptor->SetObjectField(TEXT("value_definition"), Body);
		}
		FImpl::Active->Values.Add(Value.Get(), {Value, Descriptor});
		if (Value->Type == EJson::Object) FImpl::Active->Objects.Add(Value->AsObject().Get(), Value.Get());
	}

	void FScope::RecordNameKeyedObject(const TSharedPtr<FJsonObject>& Object)
	{
		if (!FImpl::Active || !Object) return;
		UeAgentAuthoringTypeDescriptors::FDescriptor Type;
		Type.Kind = TEXT("object");
		const auto NativeType = Type.ToJsonObject();
		const auto KeyType = MakeShared<FJsonObject>();
		KeyType->SetStringField(TEXT("kind"), TEXT("name"));
		NativeType->SetObjectField(TEXT("key_type"), KeyType);
		const auto Definition = MakeShared<FJsonObject>();
		Definition->SetObjectField(TEXT("type"), NativeType);
		Record(MakeShared<FJsonValueObject>(Object), Type, Definition);
	}

	void FScope::RecordCompositeKeyedObject(const TSharedPtr<FJsonObject>& Object,
		const FString& Separator, const TArray<FString>& ComponentKinds)
	{
		if (!FImpl::Active || !Object) return;
		UeAgentAuthoringTypeDescriptors::FDescriptor Type;
		Type.Kind = TEXT("object");
		const auto NativeType = Type.ToJsonObject();
		const auto KeyType = MakeShared<FJsonObject>();
		KeyType->SetStringField(TEXT("kind"), TEXT("tuple"));
		KeyType->SetStringField(TEXT("separator"), Separator);
		TArray<TSharedPtr<FJsonValue>> Components;
		for (const auto& Kind : ComponentKinds) {
			const auto Component = MakeShared<FJsonObject>(); Component->SetStringField(TEXT("kind"), Kind);
			Components.Add(MakeShared<FJsonValueObject>(Component));
		}
		KeyType->SetArrayField(TEXT("components"), Components);
		NativeType->SetObjectField(TEXT("key_type"), KeyType);
		const auto Definition = MakeShared<FJsonObject>();
		Definition->SetObjectField(TEXT("type"), NativeType);
		Record(MakeShared<FJsonValueObject>(Object), Type, Definition);
	}

	void FScope::SetNameField(const TSharedPtr<FJsonObject>& Object, const FString& Field, FName Name)
	{
		const auto Value = MakeShared<FJsonValueString>(Name.ToString());
		Object->SetField(Field, Value);
		UeAgentAuthoringTypeDescriptors::FDescriptor Type; Type.Kind = TEXT("name");
		Record(Value, Type, nullptr);
	}

	void FScope::Copy(const TSharedPtr<FJsonValue>& Source, const TSharedPtr<FJsonValue>& Destination)
	{
		if (!FImpl::Active || !Destination) return;
		if (Source && Source->Type == EJson::Object && Destination->Type == EJson::Object)
		{
			if (const auto* Omitted = FImpl::Active->FindOmitted(Source->AsObject().Get()))
			{
				auto Copy = *Omitted;
				Copy.Parent = Destination->AsObject();
				FImpl::Active->OmittedFields.Add(Copy.Parent.Get(), MoveTemp(Copy));
			}
			if (const auto* Emissions = FImpl::Active->FindEmissions(Source->AsObject().Get()))
			{
				auto Copy = *Emissions;
				Copy.Parent = Destination->AsObject();
				FImpl::Active->FieldEmissions.Add(Copy.Parent.Get(), MoveTemp(Copy));
			}
		}
		const auto* Captured = FImpl::Active->Find(Source);
		if (!Captured) return;
		const auto Descriptor = Captured->Descriptor;
		FImpl::Active->Values.Add(Destination.Get(), {Destination, Descriptor});
		if (Destination->Type == EJson::Object)
			FImpl::Active->Objects.Add(Destination->AsObject().Get(), Destination.Get());
	}

	void FScope::SetNativeField(const TSharedPtr<FJsonObject>& Parent,
		const FString& Field, const TSharedPtr<FJsonValue>& Value, EFieldEmission Emission)
	{
		if (!Parent || Field.IsEmpty() || !Value) return;
		Parent->SetField(Field, Value);
		if (!FImpl::Active) return;
		auto& Emissions = FImpl::Active->FieldEmissions.FindOrAdd(Parent.Get());
		Emissions.Parent = Parent;
		Emissions.Fields.Add(Field, Emission);
	}

	TSharedPtr<FJsonValue> FScope::ProjectAuthoringValue(const TSharedPtr<FJsonValue>& NativeValue) const
	{
		if (!Impl->HasEmissions()) return NativeValue;
		TFunction<TSharedPtr<FJsonValue>(const TSharedPtr<FJsonValue>&)> Project;
		Project = [&](const TSharedPtr<FJsonValue>& Source) -> TSharedPtr<FJsonValue>
		{
			if (!Source) return Source;
			if (Source->Type == EJson::Object)
			{
				const auto Object = Source->AsObject();
				const auto* Emissions = Impl->FindEmissions(Object.Get());
				// Projecting descendants can grow the map; retain a value copy.
				const auto Policies = Emissions ? Emissions->Fields : TMap<FString, EFieldEmission>();
				TSharedPtr<FJsonObject> Result;
				TMap<FString, TSharedPtr<FJsonValue>> Omitted;
				for (const auto& Field : Object->Values)
				{
					const auto Value = Project(Field.Value);
					const auto* Policy = Policies.Find(Field.Key);
					const bool bOmit = Policy && (*Policy == EFieldEmission::NativeDefault
						|| (*Policy == EFieldEmission::NonEmptyObject && Value && Value->Type == EJson::Object && Value->AsObject()->Values.IsEmpty()));
					if (bOmit || Value != Field.Value)
					{
						if (!Result) { Result = MakeShared<FJsonObject>(); Result->Values = Object->Values; }
						if (bOmit) { Result->RemoveField(Field.Key); Omitted.Add(Field.Key, Field.Value); }
						else Result->SetField(Field.Key, Value);
					}
				}
				if (!Result) return Source;
				const TSharedPtr<FJsonValue> Projected = MakeShared<FJsonValueObject>(Result);
				Copy(Source, Projected);
				if (!Omitted.IsEmpty())
				{
					auto& Proof = Impl->OmittedFields.FindOrAdd(Result.Get());
					Proof.Parent = Result;
					for (const auto& Field : Omitted) Proof.Values.Add(Field.Key, Field.Value);
				}
				return Projected;
			}
			if (Source->Type == EJson::Array)
			{
				auto Items = Source->AsArray();
				bool bChanged = false;
				for (auto& Item : Items)
				{
					const auto Projected = Project(Item);
					bChanged |= Projected != Item;
					Item = Projected;
				}
				if (bChanged)
				{
					const TSharedPtr<FJsonValue> Result = MakeShared<FJsonValueArray>(MoveTemp(Items));
					Copy(Source, Result);
					return Result;
				}
			}
			return Source;
		};
		return Project(NativeValue);
	}

	TSharedPtr<FJsonObject> FScope::BuildDefinition(const TSharedPtr<FJsonValue>& Value) const
	{
		const auto Descriptor = Impl->Describe(Value);
		if (!Descriptor) return nullptr;
		const auto Result = MakeShared<FJsonObject>();
		const TSharedPtr<FJsonObject>* Definition = nullptr;
		if (Descriptor->TryGetObjectField(TEXT("value_definition"), Definition)) Result->Values = (*Definition)->Values;
		// The root can itself be a typed scalar or specialized structure.
		Result->SetObjectField(TEXT("type"), Descriptor->GetObjectField(TEXT("type")));
		return Result;
	}
}

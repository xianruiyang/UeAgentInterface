#include "Utilities/AuthoringCore/UeAgentNativeNormalizationIntent.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticCompare.h"
#include "Utilities/AuthoringObjectApply/UeAgentAuthoringObjectApply.h"
#include "Dom/JsonObject.h"

namespace UeAgentNativeNormalizationIntent
{
	namespace
	{
		thread_local FScope* ActiveScope = nullptr;
		thread_local FString ActiveTargetKey;
		thread_local const UeAgentAuthoringIdentity::FCanonicalIdentity* ActiveValueIdentity = nullptr;
		thread_local const FString* ActiveValueSemantic = nullptr;
		FString ValueKey(const FString& Semantic, const UeAgentAuthoringIdentity::FCanonicalIdentity& Identity)
		{ return Semantic.IsEmpty() ? Identity.CanonicalKey : Semantic + TEXT(":") + Identity.CanonicalKey; }
		FString DecodedPropertyKey(const FString& Owner, const FString& Type, const FString& Property)
		{ return Owner + TEXT("\n") + Type + TEXT("\n") + Property; }

		bool FailNormalization(UE::UAI::Contract::FDiagnostic& Error, const FString& Code,
			const FString& Reason, const UeAgentAuthoringIdentity::FCanonicalIdentity& Identity,
			const FString& Path = FString(), const FString& Semantic = FString())
		{
			Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason);
			Error.SourceKind = TEXT("native_result");
			Error.SubjectId = Identity.CanonicalKey;
			Error.SemanticId = Semantic;
			Error.Source.InternalJsonPath = Path;
			return false;
		}
	}

	FScope::FScope() : Previous(ActiveScope) { ActiveScope = this; }
	FScope::~FScope() { ActiveScope = Previous; }
	const FPropertyProof* FScope::Find(const UeAgentAuthoringIdentity::FCanonicalIdentity& Identity) const
	{
		return Properties.Find(Identity.CanonicalKey);
	}
	const FPropertyProof* FScope::Find(const UeAgentAuthoringState::FStateEntry& Entry) const
	{
		if (const auto* Proof = Find(Entry.TargetIdentity)) return Proof;
		return Entry.NativeTargetIdentity.IsValid() ? Find(Entry.NativeTargetIdentity) : nullptr;
	}
	FTargetScope::FTargetScope(const FString& TargetKey) : Previous(ActiveTargetKey) { ActiveTargetKey = TargetKey; }
	FTargetScope::~FTargetScope() { ActiveTargetKey = MoveTemp(Previous); }
	bool IsRecording() { return ActiveScope && !ActiveTargetKey.IsEmpty(); }
	bool IsActive() { return ActiveScope != nullptr; }
	void RecordDecodedProperties(const FString& RootOwnerPath, const FString& RootTypePath,
		const TArray<TSharedPtr<FJsonValue>>& Descriptors, const UeAgentAuthoringObjectApply::FPropertyIntent& Intent)
	{
		if (!ActiveScope || Intent.IsEmpty()) return;
		const auto Frozen = MakeShared<UeAgentAuthoringObjectApply::FPropertyIntent>(Intent);
		for (const auto& Descriptor : Descriptors)
		{
			if (!Descriptor || Descriptor->Type != EJson::Object) continue;
			const auto Object = Descriptor->AsObject();
			FString Path;
			const auto Source = Object->TryGetField(TEXT("value"));
			if (!Object->TryGetStringField(TEXT("property_path"), Path) || !Source) continue;
			FDecodedPropertyProof Proof;
			Proof.AuthoredSource = UeAgentAuthoringState::CloneCanonicalValue(Source);
			Proof.Intent = Frozen;
			ActiveScope->DecodedProperties.Add(DecodedPropertyKey(RootOwnerPath, RootTypePath, Path), MoveTemp(Proof));
		}
	}
	const FDecodedPropertyProof* FScope::FindDecodedProperty(const UeAgentAuthoringState::FReadRequest& Request) const
	{
		const auto* Type = Request.RootStruct ? Request.RootStruct
			: Request.RootObject ? Request.RootObject->GetClass() : nullptr;
		const FString Owner = Request.RootObject ? Request.RootObject->GetPathName() : Request.RootOwnerPath;
		return Type ? DecodedProperties.Find(DecodedPropertyKey(Owner, Type->GetPathName(), Request.PropertyPath)) : nullptr;
	}
	bool FinalizeValues(const UeAgentAuthoringState::FStateSnapshot& Proposed, UE::UAI::Contract::FDiagnostic& Error)
	{
		if (!ActiveScope || ActiveScope->Values.IsEmpty()) return true;
		for (const auto& Entry : Proposed.CopyEntries())
		{
			auto* Proofs = ActiveScope->Values.Find(ValueKey(Entry.SemanticId, Entry.TargetIdentity));
			if (!Proofs) Proofs = ActiveScope->Values.Find(Entry.TargetIdentity.CanonicalKey);
			if (Proofs)
			{
				for (auto& Proof : *Proofs)
				{
					if (Proof.Completion == EValueCompletion::ResolvedArgument) continue;
					const auto Value = UeAgentAuthoringSemanticCompare::FindDeclaredValuePath(Entry, Entry.Value, Proof.ProposalPath);
					if (!Value) return FailNormalization(Error, TEXT("native_normalization_readback_missing"),
						TEXT("The captured state does not contain the value required by the native normalization record."),
						Entry.TargetIdentity, Proof.ProposalPath, Entry.SemanticId);
					Proof.NormalizedValue = UeAgentAuthoringState::CloneCanonicalValue(Value);
				}
			}
		}
		return true;
	}
	bool IsValueRecording() { return ActiveScope && ActiveValueIdentity; }
	const TSharedPtr<FJsonObject>* FScope::FindKeyedArguments(const FString& RootField, const TSharedPtr<FJsonValue>& Source,
		const FString& OwnerKey) const
	{
		const FString Key = RootField + TEXT(":") + UeAgentAuthoringState::MakeStableCanonicalValueKey(Source);
		if (!OwnerKey.IsEmpty())
			if (const auto* Found = KeyedArguments.Find(OwnerKey + TEXT(":") + Key)) return Found;
		return KeyedArguments.Find(Key);
	}
	void RecordKeyedArguments(const FString& RootField, const TSharedPtr<FJsonValue>& Source,
		const TSharedPtr<FJsonObject>& Arguments, const FString& OwnerKey)
	{
		if (!ActiveScope || !Source || !Arguments) return;
		// Identical edits in two owned collections still create distinct objects.
		const FString Key = (OwnerKey.IsEmpty() ? FString() : OwnerKey + TEXT(":"))
			+ RootField + TEXT(":") + UeAgentAuthoringState::MakeStableCanonicalValueKey(Source);
		ActiveScope->KeyedArguments.Add(Key,
			UeAgentAuthoringState::CloneCanonicalValue(MakeShared<FJsonValueObject>(Arguments))->AsObject());
	}
	void RecordCreatedProperty(const UeAgentAuthoringState::FStateEntry& Normalized,
		const UeAgentAuthoringIdentity::FCanonicalIdentity& TargetIdentity, const FString& SourcePath,
		const TSharedPtr<FJsonObject>& SourceDescriptor)
	{
		if (!ActiveScope) return;
		FCreatedPropertyProof Proof;
		Proof.Entry = MakeShared<UeAgentAuthoringState::FStateEntry>(Normalized);
		Proof.Entry->TargetIdentity = TargetIdentity;
		Proof.Entry->NativeValue.Reset();
		Proof.Entry->Value = UeAgentAuthoringState::CloneCanonicalValue(Normalized.Value);
		Proof.Entry->ValueDefinition = UeAgentAuthoringState::CloneCanonicalValue(Normalized.ValueDefinition);
		Proof.Entry->ComparisonValueDefinition = UeAgentAuthoringState::CloneCanonicalValue(Normalized.ComparisonValueDefinition);
		Proof.SourcePath = SourcePath;
		if (SourceDescriptor) Proof.SourceDescriptor = UeAgentAuthoringState::CloneCanonicalValue(
			MakeShared<FJsonValueObject>(SourceDescriptor))->AsObject();
		ActiveScope->CreatedProperties.Add(MoveTemp(Proof));
	}
	FValueScope::FValueScope(const UeAgentAuthoringIdentity::FCanonicalIdentity& InIdentity, const FString& InSemanticId)
		: Identity(InIdentity), Previous(ActiveValueIdentity), SemanticId(InSemanticId), PreviousSemantic(ActiveValueSemantic)
	{ ActiveValueIdentity = &Identity; ActiveValueSemantic = &SemanticId; }
	FValueScope::~FValueScope() { ActiveValueIdentity = Previous; ActiveValueSemantic = PreviousSemantic; }
	const TArray<FValueProof>* FScope::FindValues(const UeAgentAuthoringIdentity::FCanonicalIdentity& Identity, const FString& SemanticId) const
	{
		if (const auto* Found = Values.Find(ValueKey(SemanticId, Identity))) return Found;
		return SemanticId.IsEmpty() ? nullptr : Values.Find(Identity.CanonicalKey);
	}
	bool RecordValue(const FString& Path, const TSharedPtr<FJsonValue>& SourceValue,
		const TSharedPtr<FJsonValue>& NormalizedValue, UE::UAI::Contract::FDiagnostic& Error,
		const UeAgentAuthoringTypeDescriptors::FDescriptor* Type, const TSharedPtr<FJsonValue>& ValueDefinition,
		const EValueCompletion Completion)
	{
		return RecordValue(Path, Path, SourceValue, NormalizedValue, Error, Type, ValueDefinition, Completion);
	}
	bool RecordValue(const FString& Path, const FString& ProposalPath, const TSharedPtr<FJsonValue>& SourceValue,
		const TSharedPtr<FJsonValue>& NormalizedValue, UE::UAI::Contract::FDiagnostic& Error,
		const UeAgentAuthoringTypeDescriptors::FDescriptor* Type, const TSharedPtr<FJsonValue>& ValueDefinition,
		const EValueCompletion Completion)
	{
		if (!IsValueRecording()) return true;
		if (Path.IsEmpty()) return FailNormalization(Error, TEXT("native_normalization_source_missing"),
			TEXT("The native normalization record has no value path."), *ActiveValueIdentity);
		if (!SourceValue) return FailNormalization(Error, TEXT("native_normalization_source_missing"),
			TEXT("The native normalization record is missing the source value sent to the native operation."),
			*ActiveValueIdentity, Path, ActiveValueSemantic ? *ActiveValueSemantic : FString());
		if (!NormalizedValue) return FailNormalization(Error, TEXT("native_normalization_source_missing"),
			TEXT("The native normalization record is missing the normalized result value."),
			*ActiveValueIdentity, Path, ActiveValueSemantic ? *ActiveValueSemantic : FString());
		auto& Proofs = ActiveScope->Values.FindOrAdd(ValueKey(ActiveValueSemantic ? *ActiveValueSemantic : FString(), *ActiveValueIdentity));
		auto* Proof = Proofs.FindByPredicate([&](const auto& Item) { return Item.Path == Path; });
		if (!Proof)
		{
			FValueProof Value;
			Value.Path = Path;
			Value.SourceValue = UeAgentAuthoringState::CloneCanonicalValue(SourceValue);
			if (Type) Value.Type = *Type;
			Value.ValueDefinition = UeAgentAuthoringState::CloneCanonicalValue(ValueDefinition);
			Proof = &Proofs.Add_GetRef(MoveTemp(Value));
		}
		Proof->ProposalPath = ProposalPath;
		Proof->NormalizedValue = UeAgentAuthoringState::CloneCanonicalValue(NormalizedValue);
		Proof->Completion = Completion;
		return true;
	}

	bool Record(const UeAgentAuthoringIdentity::FCanonicalIdentity& NativeIdentity,
		const TSharedPtr<FJsonValue>& SourceValue, const TSharedPtr<FJsonValue>& AuthoredSource,
		const TSharedPtr<FJsonValue>& NormalizedValue, UE::UAI::Contract::FDiagnostic& Error)
	{
		if (!IsRecording()) return true;
		const FString PropertyPath = NativeIdentity.GetSelector(TEXT("property_path"));
		if (!SourceValue) return FailNormalization(Error, TEXT("native_normalization_source_missing"),
			TEXT("The native property normalization record is missing the source value sent to the native operation."),
			NativeIdentity, PropertyPath);
		if (!AuthoredSource) return FailNormalization(Error, TEXT("native_normalization_source_missing"),
			TEXT("The native property normalization record is missing the authored input value."), NativeIdentity, PropertyPath);
		if (!NormalizedValue) return FailNormalization(Error, TEXT("native_normalization_source_missing"),
			TEXT("The native property normalization record is missing the normalized result value."), NativeIdentity, PropertyPath);
		UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
		if (!UeAgentAuthoringIdentity::BuildCanonicalIdentity(NativeIdentity.ModelId, NativeIdentity.TargetKind,
			{{TEXT("owner_path"), ActiveTargetKey},
			 {TEXT("property_path"), NativeIdentity.GetSelector(TEXT("property_path"))}}, Identity, Error)) return false;
		auto* Existing = ActiveScope->Properties.Find(Identity.CanonicalKey);
		if (!Existing)
		{
			FPropertyProof Proof;
			Proof.SourceValue = UeAgentAuthoringState::CloneCanonicalValue(SourceValue);
			Proof.AuthoredSource = UeAgentAuthoringState::CloneCanonicalValue(AuthoredSource);
			Existing = &ActiveScope->Properties.Add(Identity.CanonicalKey, MoveTemp(Proof));
		}
		Existing->NormalizedValue = UeAgentAuthoringState::CloneCanonicalValue(NormalizedValue);
		return true;
	}
}

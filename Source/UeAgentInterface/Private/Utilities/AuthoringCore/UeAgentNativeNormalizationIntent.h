#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringTypeDescriptors.h"

class FJsonValue;
class FJsonObject;
namespace UeAgentAuthoringState { class FStateSnapshot; struct FStateEntry; struct FReadRequest; }
namespace UeAgentAuthoringObjectApply { class FPropertyIntent; }

namespace UeAgentNativeNormalizationIntent
{
	enum class EValueCompletion : uint8
	{
		NativeProposal,
		// A resolved argument (for example an inferred type) is already final.
		// Reading it back from the proposal would hide an incorrect application.
		ResolvedArgument
	};

	struct FPropertyProof
	{
		TSharedPtr<FJsonValue> SourceValue;
		TSharedPtr<FJsonValue> AuthoredSource;
		TSharedPtr<FJsonValue> NormalizedValue;
	};
	struct FValueProof
	{
		FString Path;
		EValueCompletion Completion = EValueCompletion::NativeProposal;
		TSharedPtr<FJsonValue> SourceValue;
		TSharedPtr<FJsonValue> NormalizedValue;
		UeAgentAuthoringTypeDescriptors::FDescriptor Type;
		TSharedPtr<FJsonValue> ValueDefinition;
	};
	struct FCreatedPropertyProof
	{
		TSharedPtr<UeAgentAuthoringState::FStateEntry> Entry;
		FString SourcePath;
		TSharedPtr<FJsonObject> SourceDescriptor;
	};

	struct FDecodedPropertyProof
	{
		TSharedPtr<FJsonValue> AuthoredSource;
		TSharedPtr<const UeAgentAuthoringObjectApply::FPropertyIntent> Intent;
	};

	// Evidence lives only for this synchronous import. It is captured from a
	// resolved native argument or isolated proposal, never the final written object.
	class UEAGENTINTERFACE_API FScope
	{
	public:
		FScope();
		~FScope();
		FScope(const FScope&) = delete;
		FScope& operator=(const FScope&) = delete;
		const FPropertyProof* Find(const UeAgentAuthoringIdentity::FCanonicalIdentity& Identity) const;
		const FPropertyProof* Find(const UeAgentAuthoringState::FStateEntry& Entry) const;
		const TArray<FValueProof>* FindValues(const UeAgentAuthoringIdentity::FCanonicalIdentity& Identity, const FString& SemanticId = FString()) const;
		const TSharedPtr<FJsonObject>* FindKeyedArguments(const FString& RootField, const TSharedPtr<FJsonValue>& Source,
			const FString& OwnerKey = FString()) const;
		const TArray<FCreatedPropertyProof>& GetCreatedProperties() const { return CreatedProperties; }
		const FDecodedPropertyProof* FindDecodedProperty(const UeAgentAuthoringState::FReadRequest& Request) const;
	private:
		FScope* Previous;
		TMap<FString, FPropertyProof> Properties;
		TMap<FString, TArray<FValueProof>> Values;
		TMap<FString, TSharedPtr<FJsonObject>> KeyedArguments;
		TArray<FCreatedPropertyProof> CreatedProperties;
		TMap<FString, FDecodedPropertyProof> DecodedProperties;
		friend UEAGENTINTERFACE_API void RecordDecodedProperties(const FString&, const FString&,
			const TArray<TSharedPtr<FJsonValue>>&, const UeAgentAuthoringObjectApply::FPropertyIntent&);
		friend UEAGENTINTERFACE_API void RecordCreatedProperty(const UeAgentAuthoringState::FStateEntry&,
			const UeAgentAuthoringIdentity::FCanonicalIdentity&, const FString&, const TSharedPtr<FJsonObject>&);
		friend UEAGENTINTERFACE_API void RecordKeyedArguments(const FString&, const TSharedPtr<FJsonValue>&, const TSharedPtr<FJsonObject>&, const FString&);
		friend UEAGENTINTERFACE_API bool Record(const UeAgentAuthoringIdentity::FCanonicalIdentity&,
			const TSharedPtr<FJsonValue>&, const TSharedPtr<FJsonValue>&,
			const TSharedPtr<FJsonValue>&, UE::UAI::Contract::FDiagnostic&);
		friend UEAGENTINTERFACE_API bool RecordValue(const FString&, const TSharedPtr<FJsonValue>&,
			const TSharedPtr<FJsonValue>&, UE::UAI::Contract::FDiagnostic&, const UeAgentAuthoringTypeDescriptors::FDescriptor*, const TSharedPtr<FJsonValue>&, EValueCompletion);
		friend UEAGENTINTERFACE_API bool FinalizeValues(const UeAgentAuthoringState::FStateSnapshot&, UE::UAI::Contract::FDiagnostic&);
	};
	// Bind only the isolated proposal's native execution. The frozen value must
	// be the argument actually sent to the native owner, not its domain DTO.
	class UEAGENTINTERFACE_API FValueScope
	{
	public:
		explicit FValueScope(const UeAgentAuthoringIdentity::FCanonicalIdentity& Identity, const FString& SemanticId = FString());
		~FValueScope();
		FValueScope(const FValueScope&) = delete;
		FValueScope& operator=(const FValueScope&) = delete;
	private:
		UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
		const UeAgentAuthoringIdentity::FCanonicalIdentity* Previous;
		FString SemanticId;
		const FString* PreviousSemantic;
	};

	// Bind the preview to the actual native root used by the final export,
	// preserving the distinction between object and operation/package identity.
	class UEAGENTINTERFACE_API FTargetScope
	{
	public:
		explicit FTargetScope(const FString& TargetKey);
		~FTargetScope();
		FTargetScope(const FTargetScope&) = delete;
		FTargetScope& operator=(const FTargetScope&) = delete;
	private:
		FString Previous;
	};

	UEAGENTINTERFACE_API bool Record(
		const UeAgentAuthoringIdentity::FCanonicalIdentity& NativeIdentity,
		const TSharedPtr<FJsonValue>& SourceValue,
		const TSharedPtr<FJsonValue>& AuthoredSource,
		const TSharedPtr<FJsonValue>& NormalizedValue, UE::UAI::Contract::FDiagnostic& Error);
	UEAGENTINTERFACE_API bool IsRecording();
	UEAGENTINTERFACE_API bool IsActive();
	UEAGENTINTERFACE_API bool IsValueRecording();
	/** Keep the input consumed by ObjectApply together with its independently
	 * decoded intent. Final readback reuses that owner, never decodes apply metadata. */
	UEAGENTINTERFACE_API void RecordDecodedProperties(const FString& RootOwnerPath, const FString& RootTypePath,
		const TArray<TSharedPtr<FJsonValue>>& Descriptors, const UeAgentAuthoringObjectApply::FPropertyIntent& Intent);
	UEAGENTINTERFACE_API bool RecordValue(const FString& Path,
		const TSharedPtr<FJsonValue>& SourceValue, const TSharedPtr<FJsonValue>& NormalizedValue, UE::UAI::Contract::FDiagnostic& Error,
		const UeAgentAuthoringTypeDescriptors::FDescriptor* Type = nullptr, const TSharedPtr<FJsonValue>& ValueDefinition = nullptr,
		EValueCompletion Completion = EValueCompletion::NativeProposal);
	UEAGENTINTERFACE_API bool FinalizeValues(const UeAgentAuthoringState::FStateSnapshot& Proposed, UE::UAI::Contract::FDiagnostic& Error);
	// Constructor arguments are frozen before publication, keyed by the immutable
	// semantic request. They supply creation defaults and native identity only.
	UEAGENTINTERFACE_API void RecordKeyedArguments(const FString& RootField,
		const TSharedPtr<FJsonValue>& Source, const TSharedPtr<FJsonObject>& Arguments,
		const FString& OwnerKey = FString());
	/** Only explicit constructor property edits, decoded and verified by the common
	 * native codec. No transient template storage survives to final readback. */
	UEAGENTINTERFACE_API void RecordCreatedProperty(const UeAgentAuthoringState::FStateEntry& Normalized,
		const UeAgentAuthoringIdentity::FCanonicalIdentity& TargetIdentity, const FString& SourcePath,
		const TSharedPtr<FJsonObject>& SourceDescriptor = nullptr);
}

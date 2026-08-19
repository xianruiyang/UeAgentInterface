#include "Misc/AutomationTest.h"

// RE-007 production must satisfy exactly the corrected seam frozen by RE-006.
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadNative.h"

#include "Animation/AnimBlueprint.h"
#include "Core/ContextResource/UeAgentOperationContext.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Engine/Blueprint.h"
#include "Materials/Material.h"
#include "Engine/Texture2D.h"
#include "NiagaraSystem.h"
#include "PCGGraph.h"
#include "Sound/SoundCue.h"
#include "UObject/Package.h"

#include <type_traits>
#include <utility>

namespace
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::Discovery;
	using namespace UE::UAI::DomainAdapters::ContentGraphRead;
	using namespace UE::UAI::StateSemantic;

	const TArray<FString> AllSemanticPrefixes = {
		TEXT("content_graph.summary:"),
		TEXT("content_graph.container:"),
		TEXT("content_graph.item:"),
		TEXT("content_graph.relation:"),
		TEXT("content_graph.property:"),
		TEXT("content_graph.parameter:"),
		TEXT("content_graph.diagnostic:"),
	};

	template <typename T, typename = void>
	struct TContentGraphStateMember
	{
		using Type = void;
		static constexpr bool Exists = false;
	};

	template <typename T>
	struct TContentGraphStateMember<
		T,
		std::void_t<decltype(std::declval<T&>().State)>>
	{
		using Type = std::decay_t<
			decltype(std::declval<T&>().State)>;
		static constexpr bool Exists = true;
	};

	template <typename T, typename = void>
	struct TCanonicalStateShape : std::false_type
	{
	};

	template <typename T>
	struct TCanonicalStateShape<
		T,
		std::void_t<
			decltype(std::declval<T&>().Type),
			decltype(std::declval<T&>().Value),
			decltype(std::declval<T&>().Default),
			decltype(std::declval<T&>().AuthoredPresence),
			decltype(std::declval<T&>().OverrideState)>>
		: std::bool_constant<
			std::is_same_v<
				std::decay_t<decltype(std::declval<T&>().Type)>,
				TOptional<FUeAgentCanonicalType>>
			&& std::is_same_v<
				std::decay_t<decltype(std::declval<T&>().Value)>,
				FUeAgentCanonicalValuePtr>
			&& std::is_same_v<
				std::decay_t<decltype(std::declval<T&>().Default)>,
				TOptional<FUeAgentDefaultValue>>
			&& std::is_same_v<
				std::decay_t<
					decltype(
						std::declval<T&>().AuthoredPresence)>,
				EUeAgentAuthoredPresence>
			&& std::is_same_v<
				std::decay_t<
					decltype(std::declval<T&>().OverrideState)>,
				EUeAgentOverrideState>>
	{
	};

	template <typename T, typename = void>
	struct THasLegacyPropertyStrings : std::false_type
	{
	};

	template <typename T>
	struct THasLegacyPropertyStrings<
		T,
		std::void_t<
			decltype(std::declval<T&>().Value),
			decltype(std::declval<T&>().DefaultValue),
			decltype(std::declval<T&>().bHasDefault)>>
		: std::bool_constant<
			std::is_same_v<
				std::decay_t<decltype(std::declval<T&>().Value)>,
				FString>
			&& std::is_same_v<
				std::decay_t<
					decltype(std::declval<T&>().DefaultValue)>,
				FString>>
	{
	};

	template <typename T, typename = void>
	struct THasLegacyParameterStrings : std::false_type
	{
	};

	template <typename T>
	struct THasLegacyParameterStrings<
		T,
		std::void_t<
			decltype(std::declval<T&>().TypeId),
			decltype(std::declval<T&>().Value)>>
		: std::bool_constant<
			std::is_same_v<
				std::decay_t<decltype(std::declval<T&>().TypeId)>,
				FString>
			&& std::is_same_v<
				std::decay_t<decltype(std::declval<T&>().Value)>,
				FString>>
	{
	};

	template <typename T>
	void AssignCanonicalState(
		T& Fact,
		const FUeAgentCanonicalType& Type,
		const FUeAgentCanonicalValuePtr& Value,
		const FUeAgentDefaultValue& Default,
		EUeAgentAuthoredPresence AuthoredPresence,
		EUeAgentOverrideState OverrideState)
	{
		using FState =
			typename TContentGraphStateMember<T>::Type;
		if constexpr (TCanonicalStateShape<FState>::value)
		{
			Fact.State.Type = Type;
			Fact.State.Value = Value;
			Fact.State.Default = Default;
			Fact.State.AuthoredPresence = AuthoredPresence;
			Fact.State.OverrideState = OverrideState;
		}
	}

	template <typename T>
	T MakePropertyFact(
		const FString& OwnerId,
		const FString& FieldId,
		const FUeAgentCanonicalType& Type,
		const FUeAgentCanonicalValuePtr& Value,
		const FUeAgentDefaultValue& Default,
		EUeAgentAuthoredPresence AuthoredPresence,
		EUeAgentOverrideState OverrideState)
	{
		T Result{};
		Result.OwnerId = OwnerId;
		Result.FieldId = FieldId;
		AssignCanonicalState(
			Result,
			Type,
			Value,
			Default,
			AuthoredPresence,
			OverrideState);
		if constexpr (THasLegacyPropertyStrings<T>::value)
		{
			Result.Value = TEXT("__legacy_stringified_object__");
			Result.DefaultValue =
				TEXT("__legacy_stringified_default__");
			Result.bHasDefault = true;
		}
		return Result;
	}

	template <typename T>
	T MakeParameterFact(
		const FString& ParameterId,
		const FString& OwnerId,
		const FUeAgentCanonicalType& Type,
		const FUeAgentCanonicalValuePtr& Value,
		const FUeAgentDefaultValue& Default,
		EUeAgentAuthoredPresence AuthoredPresence,
		EUeAgentOverrideState OverrideState)
	{
		T Result{};
		Result.ParameterId = ParameterId;
		Result.OwnerId = OwnerId;
		AssignCanonicalState(
			Result,
			Type,
			Value,
			Default,
			AuthoredPresence,
			OverrideState);
		if constexpr (THasLegacyParameterStrings<T>::value)
		{
			Result.TypeId = TEXT("core.string");
			Result.Value = TEXT("__legacy_stringified_array__");
		}
		return Result;
	}

	struct FOwnedGraph
	{
		FString Domain;
		UObject* Owner = nullptr;
		UObject* ItemObject = nullptr;
		FString ContainerId;
		FString ItemId;
	};

	struct FCanonicalFixtureValues
	{
		FUeAgentCanonicalValuePtr Configuration;
		FUeAgentCanonicalValuePtr ConfigurationDefault;
		FUeAgentCanonicalValuePtr Parameter;
		FUeAgentCanonicalValuePtr ParameterDefault;
	};

	enum class EInvalidFieldStateCase : uint8
	{
		None,
		MissingType,
		MissingValue,
		MissingDefault,
		TypeMismatch,
		OverrideWithoutAuthoredPresence,
		ParameterMissingDefault,
	};

	bool TryBuildCanonicalFixtureValues(
		const FOwnedGraph& Graph,
		FUeAgentResourceTracker& ResourceTracker,
		FCanonicalFixtureValues& OutValues,
		FUeAgentContentGraphReadError& OutError)
	{
		FUeAgentStateSemanticError Error;
		auto Failed =
			[&OutError, &Error](const TCHAR* Stage)
			{
				OutError.Code =
					Error.Code.IsEmpty()
						? TEXT("fixture_canonical_value_failed")
						: Error.Code;
				OutError.SubjectId = Stage;
				OutError.Detail = Error.Detail;
				return false;
			};

		FUeAgentCanonicalValuePtr CanonicalNull;
		if (!FUeAgentCanonicalValue::TryNull(
				ResourceTracker,
				CanonicalNull,
				Error))
		{
			return Failed(TEXT("null"));
		}
		FUeAgentCanonicalValuePtr Enabled;
		FUeAgentCanonicalValuePtr Disabled;
		if (!FUeAgentCanonicalValue::TryBoolean(
				true,
				ResourceTracker,
				Enabled,
				Error)
			|| !FUeAgentCanonicalValue::TryBoolean(
				false,
				ResourceTracker,
				Disabled,
				Error))
		{
			return Failed(TEXT("boolean"));
		}
		FUeAgentCanonicalValuePtr ExactCount;
		FUeAgentCanonicalValuePtr DefaultCount;
		if (!FUeAgentCanonicalValue::TryInteger(
				TEXT("9007199254740993"),
				ResourceTracker,
				ExactCount,
				Error)
			|| !FUeAgentCanonicalValue::TryInteger(
				TEXT("0"),
				ResourceTracker,
				DefaultCount,
				Error))
		{
			return Failed(TEXT("integer"));
		}
		FUeAgentCanonicalValuePtr Alpha;
		FUeAgentCanonicalValuePtr Beta;
		FUeAgentCanonicalValuePtr ReferenceKind;
		FUeAgentCanonicalValuePtr ReferencePath;
		if (!FUeAgentCanonicalValue::TryString(
				TEXT("alpha"),
				ResourceTracker,
				Alpha,
				Error)
			|| !FUeAgentCanonicalValue::TryString(
				TEXT("beta"),
				ResourceTracker,
				Beta,
				Error)
			|| !FUeAgentCanonicalValue::TryString(
				TEXT("uobject_reference"),
				ResourceTracker,
				ReferenceKind,
				Error)
			|| !FUeAgentCanonicalValue::TryString(
				Graph.ItemObject->GetPathName(),
				ResourceTracker,
				ReferencePath,
				Error))
		{
			return Failed(TEXT("string"));
		}
		FUeAgentCanonicalValuePtr Labels;
		FUeAgentCanonicalValuePtr EmptyArray;
		if (!FUeAgentCanonicalValue::TryArray(
				{Alpha.ToSharedRef(), Beta.ToSharedRef()},
				ResourceTracker,
				Labels,
				Error)
			|| !FUeAgentCanonicalValue::TryArray(
				{},
				ResourceTracker,
				EmptyArray,
				Error))
		{
			return Failed(TEXT("array"));
		}
		FUeAgentCanonicalValuePtr Reference;
		if (!FUeAgentCanonicalValue::TryObject(
				{
					{TEXT("kind"), ReferenceKind.ToSharedRef()},
					{TEXT("path"), ReferencePath.ToSharedRef()},
				},
				ResourceTracker,
				Reference,
				Error))
		{
			return Failed(TEXT("reference"));
		}
		if (!FUeAgentCanonicalValue::TryObject(
				{
					{TEXT("enabled"), Enabled.ToSharedRef()},
					{TEXT("exact_count"), ExactCount.ToSharedRef()},
					{TEXT("labels"), Labels.ToSharedRef()},
					{TEXT("optional"), CanonicalNull.ToSharedRef()},
					{TEXT("target"), Reference.ToSharedRef()},
				},
				ResourceTracker,
				OutValues.Configuration,
				Error)
			|| !FUeAgentCanonicalValue::TryObject(
				{
					{TEXT("enabled"), Disabled.ToSharedRef()},
					{
						TEXT("exact_count"),
						DefaultCount.ToSharedRef(),
					},
					{TEXT("labels"), EmptyArray.ToSharedRef()},
					{TEXT("optional"), CanonicalNull.ToSharedRef()},
					{TEXT("target"), CanonicalNull.ToSharedRef()},
				},
				ResourceTracker,
				OutValues.ConfigurationDefault,
				Error))
		{
			return Failed(TEXT("configuration"));
		}
		if (!FUeAgentCanonicalValue::TryArray(
				{
					ExactCount.ToSharedRef(),
					Reference.ToSharedRef(),
					CanonicalNull.ToSharedRef(),
				},
				ResourceTracker,
				OutValues.Parameter,
				Error)
			|| !FUeAgentCanonicalValue::TryArray(
				{},
				ResourceTracker,
				OutValues.ParameterDefault,
				Error))
		{
			return Failed(TEXT("parameter"));
		}
		return true;
	}

	class FOwnedContentGraphs final
	{
	public:
		FOwnedContentGraphs()
		{
			const FString PackageName = FString::Printf(
				TEXT("/Temp/UAI/ContentGraphRead/%s"),
				*FGuid::NewGuid().ToString(EGuidFormats::Digits));
			Package = CreatePackage(*PackageName);
			Add<UBlueprint>(TEXT("blueprint"), TEXT("OwnedBlueprint"));
			Add<UMaterial>(TEXT("material"), TEXT("OwnedMaterial"));
			Add<UNiagaraSystem>(TEXT("niagara"), TEXT("OwnedNiagara"));
			Add<UPCGGraph>(TEXT("pcg"), TEXT("OwnedPcg"));
			Add<UAnimBlueprint>(
				TEXT("animation"),
				TEXT("OwnedAnimation"));
			Add<USoundCue>(TEXT("audio"), TEXT("OwnedAudio"));
			Package->SetDirtyFlag(false);
		}

		~FOwnedContentGraphs()
		{
			if (Package)
			{
				Package->SetDirtyFlag(false);
				Package->MarkAsGarbage();
			}
		}

		const FOwnedGraph* Find(const FString& BindingId) const
		{
			return Graphs.FindByPredicate(
				[&BindingId](const FOwnedGraph& Graph)
				{
					return BindingId == TEXT("owned-") + Graph.Domain;
				});
		}

		UPackage* Package = nullptr;
		TArray<FOwnedGraph> Graphs;

	private:
		template <typename TOwner>
		void Add(const TCHAR* Domain, const TCHAR* Name)
		{
			TOwner* Owner = NewObject<TOwner>(
				Package,
				Name,
				RF_Public | RF_Standalone);
			UObject* ItemObject = NewObject<UTexture2D>(
				Owner,
				*FString::Printf(TEXT("%sItem"), Name),
				RF_Transient);
			Graphs.Add({
				Domain,
				Owner,
				ItemObject,
				FString(TEXT("container.")) + Domain,
				FString(TEXT("item.")) + Domain,
			});
		}
	};

	FUeAgentDescriptorIdentity Identity()
	{
		return FUeAgentDescriptorIdentity(
			TEXT("uai.query.content_graph.compound"),
			1,
			TEXT(
				"sha256:5115bd528cefb896dccc1a6f5861f0e"
				"2fe99d12ce218790184c16571903a2f36"));
	}

	FUeAgentResourceTrackerOwner Resources()
	{
		return MakeShared<FUeAgentResourceTracker, ESPMode::ThreadSafe>(
			FUeAgentResourceBudget({
				{EUeAgentResourceDimension::InputBytes, 1024 * 1024},
				{EUeAgentResourceDimension::OutputBytes, 1024 * 1024},
				{EUeAgentResourceDimension::Nodes, 100000},
				{EUeAgentResourceDimension::Items, 100000},
				{EUeAgentResourceDimension::StringBytes, 1024 * 1024},
				{EUeAgentResourceDimension::WorkUnits, 100000},
				{
					EUeAgentResourceDimension::EstimatedMemoryBytes,
					8 * 1024 * 1024,
				},
			}));
	}

	TSharedPtr<FUeAgentOperationContext> Context(
		FAutomationTestBase* Test,
		const FUeAgentResourceTrackerOwner& ResourceTracker)
	{
		FUeAgentExecutionContextInput Input;
		Input.ProjectInstanceId = TEXT("owned-content-graph-project");
		Input.EditorInstanceId = TEXT("owned-content-graph-editor");
		Input.StatePlane = EUeAgentStatePlane::AuthoringMemory;
		Input.WorldType = EUeAgentExecutionWorldType::Editor;
		Input.ContextRevision = TEXT("owned-content-graph-revision");
		const FUeAgentExecutionContextPolicy Policy(
			{EUeAgentStatePlane::AuthoringMemory},
			{},
			EUeAgentContextResolutionPolicy::ResolveCurrentThenFreeze,
			EUeAgentTransientContextPolicy::ReadOnly);
		FUeAgentExecutionContextRefPtr ExecutionContext;
		FUeAgentExecutionContextError ContextError;
		Test->TestTrue(
			TEXT("freeze owned content graph context"),
			FUeAgentExecutionContextRef::TryCreate(
				Input,
				Identity(),
				EUeAgentRequestKind::Query,
				Policy,
				ExecutionContext,
				ContextError));
		if (!ExecutionContext.IsValid())
		{
			return nullptr;
		}
		const FUeAgentQueryRequest Request(
			Identity(),
			TEXT("request-content-graph-read-native"),
			FUeAgentContractValue(
				TEXT("{\"sections\":[\"summary\",\"structure\","
					"\"items\",\"relations\",\"properties\","
					"\"parameters\",\"diagnostics\"]}")));
		const FUeAgentCallerContext Caller(
			TEXT("fixture-agent"),
			{TEXT("content_graph:read")},
			TEXT("corr-content-graph-read-native"));
		const FUeAgentOperationDeadline Deadline =
			FUeAgentOperationDeadline::FromTimeout(
				30.0,
				[]() { return 100.0; });
		const FUeAgentCancellationSource Cancellation(
			[]() { return 100.0; });
		TSharedPtr<FUeAgentOperationContext> Result;
		FUeAgentOperationContextError Error;
		Test->TestTrue(
			TEXT("create native operation context"),
			FUeAgentOperationContext::TryCreate(
				Request,
				Caller,
				ExecutionContext.ToSharedRef(),
				ResourceTracker,
				Deadline,
				Cancellation.GetToken(),
				Result,
				Error));
		return Result;
	}

	FUeAgentCandidateBinding Target(const FOwnedGraph& Graph)
	{
		const FString BindingId = TEXT("owned-") + Graph.Domain;
		return FUeAgentCandidateBinding(
			BindingId,
			FUeAgentCandidateSummary(
				FUeAgentCanonicalIdentity(
					TEXT("content_graph.target:") + BindingId,
					Graph.Domain,
					Graph.Owner->GetPathName(),
					Graph.Owner->GetName(),
					TEXT("target"),
					TEXT("owned-content-graph-revision")),
				Identity(),
				Graph.Domain,
				Graph.Owner->GetName(),
				TEXT("owned real UE content graph"),
				{TEXT("ContentGraphRead"), Graph.Domain}),
			FUeAgentCatalogRevision(
				TEXT("owned-content-graph-catalog"),
				TEXT("owned-content-graph-revision"),
				1),
			Graph.Owner->GetPathName(),
			TEXT("content-graph-read"),
			{EUeAgentCandidateOperation::Read},
			TEXT("content-graph-read-native-test"));
	}

	class FOwnedEnvironment final
		: public IUeAgentContentGraphReadEnvironment
	{
	public:
		explicit FOwnedEnvironment(FOwnedContentGraphs& InOwner)
			: Owner(InOwner)
			, CanonicalResources(Resources())
		{
		}

		bool TryCapture(
			const FUeAgentCandidateBinding& InTarget,
			const FUeAgentExecutionContextRef& InContext,
			FUeAgentContentGraphCapture& OutCapture,
			FUeAgentContentGraphReadError& OutError) override
		{
			++Calls;
			if (bFail)
			{
				OutError.Code =
					TEXT("fixture_content_graph_capture_failed");
				OutError.SubjectId = InTarget.GetBindingId();
				OutError.Detail = TEXT("fixture environment failure");
				return false;
			}
			const FOwnedGraph* Graph = Owner.Find(
				InTarget.GetBindingId());
			if (!Graph
				|| InContext.GetProjectInstanceId()
					!= TEXT("owned-content-graph-project"))
			{
				OutError.Code =
					TEXT("content_graph_read_target_unresolved");
				OutError.SubjectId = InTarget.GetBindingId();
				OutError.Detail = TEXT("owned graph target not found");
				return false;
			}
			OutCapture.Owner = Graph->Owner;
			OutCapture.DomainId = Graph->Domain;
			OutCapture.Revision = TEXT("owned-content-graph-revision");
			OutCapture.Containers = {
				{
					Graph->ContainerId,
					TEXT("graph"),
					Graph->Owner->GetPathName(),
					FString(),
					0,
				},
			};
			OutCapture.Items = {
				{
					Graph->ItemId,
					TEXT("fixture_item"),
					Graph->Owner->GetPathName(),
					Graph->ItemObject->GetClass()->GetPathName(),
					Graph->ContainerId,
					0,
				},
			};
			OutCapture.Relations = {
				{
					TEXT("relation.") + Graph->Domain,
					TEXT("membership"),
					Graph->ContainerId,
					Graph->ItemId,
				},
			};
			FCanonicalFixtureValues Values;
			if (!TryBuildCanonicalFixtureValues(
					*Graph,
					*CanonicalResources,
					Values,
					OutError))
			{
				return false;
			}
			const FUeAgentCanonicalType ConfigurationType(
				TEXT("fixture.content_graph.configuration"),
				EUeAgentValueKind::Object,
				false,
				TEXT("object"));
			const FUeAgentCanonicalType ParameterType(
				TEXT("fixture.content_graph.parameter_list"),
				EUeAgentValueKind::Array,
				false,
				TEXT("array"));
			OutCapture.Properties = {
				MakePropertyFact<FUeAgentContentGraphProperty>(
					Graph->ItemId,
					TEXT("configuration"),
					ConfigurationType,
					Values.Configuration,
					FUeAgentDefaultValue(
						EUeAgentDefaultSourceKind::
							ClassDefaultObject,
						Graph->Owner->GetPathName(),
						TEXT("fixture-effective-default"),
						Values.ConfigurationDefault.ToSharedRef()),
					EUeAgentAuthoredPresence::Present,
					EUeAgentOverrideState::Overridden),
			};
			OutCapture.Parameters = {
				MakeParameterFact<FUeAgentContentGraphParameter>(
					TEXT("parameter.") + Graph->Domain,
					Graph->ItemId,
					ParameterType,
					Values.Parameter,
					FUeAgentDefaultValue(
						EUeAgentDefaultSourceKind::Schema,
						TEXT("fixture.parameter.schema"),
						TEXT("fixture-effective-default"),
						Values.ParameterDefault.ToSharedRef()),
					EUeAgentAuthoredPresence::Present,
					EUeAgentOverrideState::Overridden),
			};
			switch (InvalidFieldState)
			{
			case EInvalidFieldStateCase::MissingType:
				OutCapture.Properties[0].State.Type.Reset();
				break;
			case EInvalidFieldStateCase::MissingValue:
				OutCapture.Properties[0].State.Value.Reset();
				break;
			case EInvalidFieldStateCase::MissingDefault:
				OutCapture.Properties[0].State.Default.Reset();
				break;
			case EInvalidFieldStateCase::TypeMismatch:
				OutCapture.Properties[0].State.Type =
					FUeAgentCanonicalType(
						TEXT("core.string"),
						EUeAgentValueKind::String,
						false,
						TEXT("scalar"));
				break;
			case EInvalidFieldStateCase::
				OverrideWithoutAuthoredPresence:
				OutCapture.Properties[0]
					.State.AuthoredPresence =
						EUeAgentAuthoredPresence::Absent;
				break;
			case EInvalidFieldStateCase::ParameterMissingDefault:
				OutCapture.Parameters[0].State.Default.Reset();
				break;
			case EInvalidFieldStateCase::None:
			default:
				break;
			}
			OutCapture.Diagnostics = {
				{
					TEXT("diagnostic.") + Graph->Domain,
					Graph->Owner->GetPathName(),
					TEXT("warning"),
					TEXT("fixture_warning"),
					TEXT("owned fixture diagnostic"),
					false,
				},
			};
			return true;
		}

		bool bFail = false;
		EInvalidFieldStateCase InvalidFieldState =
			EInvalidFieldStateCase::None;
		int32 Calls = 0;

	private:
		FOwnedContentGraphs& Owner;
		FUeAgentResourceTrackerOwner CanonicalResources;
	};

	bool Capture(
		FAutomationTestBase* Test,
		const FString& ProviderBinding,
		const FString& Input,
		const TOptional<FUeAgentCandidateBinding>& InTarget,
		const TSharedRef<IUeAgentContentGraphReadEnvironment>& Environment,
		const FUeAgentOperationContext& OperationContext,
		TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
		FUeAgentContentGraphReadError& OutError)
	{
		TSharedPtr<IUeAgentContentGraphReadHook> Hook;
		if (!Test->TestTrue(
				*FString::Printf(
					TEXT("%s hook exists"),
					*ProviderBinding),
				FUeAgentContentGraphReadNativeHookFactory::TryCreate(
					ProviderBinding,
					Environment,
					Hook,
					OutError)))
		{
			return false;
		}
		return Hook.IsValid()
			&& Hook->TryCapture(
				FUeAgentContractValue(Input),
				InTarget,
				OperationContext,
				OutSnapshot,
				OutError);
	}

	bool HasOwner(
		const TSharedPtr<const FUeAgentCanonicalSnapshot>& Snapshot,
		const FString& OwnerId)
	{
		return Snapshot.IsValid()
			&& Snapshot->GetProvenance().GetEvidenceId() == OwnerId;
	}

	bool HasSemanticPrefix(
		const TSharedPtr<const FUeAgentCanonicalSnapshot>& Snapshot,
		const FString& Prefix)
	{
		return Snapshot.IsValid()
			&& Snapshot->GetEntries().ContainsByPredicate(
				[&Prefix](const FUeAgentStateEntry& Entry)
				{
					return Entry.GetIdentity()
						.GetSemanticId()
						.StartsWith(Prefix);
				});
	}

	const FUeAgentStateEntry* FindEntry(
		const TSharedPtr<const FUeAgentCanonicalSnapshot>& Snapshot,
		const FString& SemanticId)
	{
		if (!Snapshot.IsValid())
		{
			return nullptr;
		}
		return Snapshot->GetEntries().FindByPredicate(
			[&SemanticId](const FUeAgentStateEntry& Entry)
			{
				return Entry.GetIdentity().GetSemanticId()
					== SemanticId;
			});
	}

	const FUeAgentCanonicalValueRef* FindObjectField(
		const FUeAgentCanonicalValueRef& Object,
		const FString& Field)
	{
		if (Object->GetKind() != EUeAgentValueKind::Object)
		{
			return nullptr;
		}
		const TPair<FString, FUeAgentCanonicalValueRef>* Item =
			Object->GetObjectItems().FindByPredicate(
				[&Field](
					const TPair<
						FString,
						FUeAgentCanonicalValueRef>& Candidate)
				{
					return Candidate.Key == Field;
				});
		return Item ? &Item->Value : nullptr;
	}

	bool VerifyCaptureStateContract(FAutomationTestBase* Test)
	{
		using FPropertyState =
			typename TContentGraphStateMember<
				FUeAgentContentGraphProperty>::Type;
		using FParameterState =
			typename TContentGraphStateMember<
				FUeAgentContentGraphParameter>::Type;
		bool bValid = Test->TestTrue(
			TEXT(
				"property owns a canonical type/value/default/"
				"presence/override state carrier"),
			TContentGraphStateMember<
				FUeAgentContentGraphProperty>::Exists
				&& TCanonicalStateShape<FPropertyState>::value);
		bValid = Test->TestTrue(
			TEXT(
				"parameter owns the same canonical state "
				"carrier contract"),
			TContentGraphStateMember<
				FUeAgentContentGraphParameter>::Exists
				&& TCanonicalStateShape<FParameterState>::value)
			&& bValid;
		bValid = Test->TestTrue(
			TEXT(
				"property and parameter reuse one canonical "
				"state carrier type"),
			TContentGraphStateMember<
				FUeAgentContentGraphProperty>::Exists
				&& TContentGraphStateMember<
					FUeAgentContentGraphParameter>::Exists
				&& std::is_same_v<
					FPropertyState,
					FParameterState>)
			&& bValid;
		bValid = Test->TestFalse(
			TEXT(
				"property capture has no legacy string "
				"value/default seam"),
			THasLegacyPropertyStrings<
				FUeAgentContentGraphProperty>::value)
			&& bValid;
		bValid = Test->TestFalse(
			TEXT(
				"parameter capture has no legacy string "
				"type/value seam"),
			THasLegacyParameterStrings<
				FUeAgentContentGraphParameter>::value)
			&& bValid;
		return bValid;
	}

	bool VerifyConfigurationValue(
		FAutomationTestBase* Test,
		const FUeAgentCanonicalValueRef& Value,
		const FOwnedGraph& Graph)
	{
		bool bValid = Test->TestEqual(
			TEXT("configuration remains a canonical object"),
			Value->GetKind(),
			EUeAgentValueKind::Object);
		if (Value->GetKind() != EUeAgentValueKind::Object)
		{
			return false;
		}

		const FUeAgentCanonicalValueRef* Enabled =
			FindObjectField(Value, TEXT("enabled"));
		bValid = Test->TestNotNull(
			TEXT("configuration retains boolean field"),
			Enabled)
			&& bValid;
		if (Enabled)
		{
			bValid = Test->TestEqual(
				TEXT("configuration boolean kind is exact"),
				(*Enabled)->GetKind(),
				EUeAgentValueKind::Boolean)
				&& bValid;
			bValid = Test->TestTrue(
				TEXT("configuration boolean value is exact"),
				(*Enabled)->GetBoolean())
				&& bValid;
		}

		const FUeAgentCanonicalValueRef* ExactCount =
			FindObjectField(Value, TEXT("exact_count"));
		bValid = Test->TestNotNull(
			TEXT("configuration retains integer field"),
			ExactCount)
			&& bValid;
		if (ExactCount)
		{
			bValid = Test->TestEqual(
				TEXT("configuration integer kind is exact"),
				(*ExactCount)->GetKind(),
				EUeAgentValueKind::Integer)
				&& bValid;
			bValid = Test->TestEqual(
				TEXT(
					"configuration integer lexeme is not "
						"rounded or coerced"),
				(*ExactCount)->GetLexeme(),
				FString(TEXT("9007199254740993")))
				&& bValid;
		}

		const FUeAgentCanonicalValueRef* Labels =
			FindObjectField(Value, TEXT("labels"));
		bValid = Test->TestNotNull(
			TEXT("configuration retains array field"),
			Labels)
			&& bValid;
		if (Labels)
		{
			bValid = Test->TestEqual(
				TEXT("configuration array kind is exact"),
				(*Labels)->GetKind(),
				EUeAgentValueKind::Array)
				&& bValid;
			bValid = Test->TestEqual(
				TEXT("configuration array order is retained"),
				(*Labels)->GetArrayItems().Num(),
				2)
				&& bValid;
			if ((*Labels)->GetArrayItems().Num() == 2)
			{
				bValid = Test->TestEqual(
					TEXT("configuration first array item"),
					(*Labels)->GetArrayItems()[0]->GetText(),
					FString(TEXT("alpha")))
					&& bValid;
				bValid = Test->TestEqual(
					TEXT("configuration second array item"),
					(*Labels)->GetArrayItems()[1]->GetText(),
					FString(TEXT("beta")))
					&& bValid;
			}
		}

		const FUeAgentCanonicalValueRef* Optional =
			FindObjectField(Value, TEXT("optional"));
		bValid = Test->TestNotNull(
			TEXT("configuration retains canonical null field"),
			Optional)
			&& bValid;
		if (Optional)
		{
			bValid = Test->TestEqual(
				TEXT("canonical null is not absent or string null"),
				(*Optional)->GetKind(),
				EUeAgentValueKind::Null)
				&& bValid;
		}

		const FUeAgentCanonicalValueRef* Target =
			FindObjectField(Value, TEXT("target"));
		bValid = Test->TestNotNull(
			TEXT("configuration retains structured reference"),
			Target)
			&& bValid;
		if (Target
			&& (*Target)->GetKind() == EUeAgentValueKind::Object)
		{
			const FUeAgentCanonicalValueRef* Kind =
				FindObjectField(*Target, TEXT("kind"));
			const FUeAgentCanonicalValueRef* Path =
				FindObjectField(*Target, TEXT("path"));
			bValid = Test->TestNotNull(
				TEXT("reference kind is present"),
				Kind)
				&& bValid;
			bValid = Test->TestNotNull(
				TEXT("reference path is present"),
				Path)
				&& bValid;
			if (Kind)
			{
				bValid = Test->TestEqual(
					TEXT("reference kind is exact"),
					(*Kind)->GetText(),
					FString(TEXT("uobject_reference")))
					&& bValid;
			}
			if (Path)
			{
				bValid = Test->TestEqual(
					TEXT("reference path is exact"),
					(*Path)->GetText(),
					Graph.ItemObject->GetPathName())
					&& bValid;
			}
		}
		else
		{
			bValid = Test->TestTrue(
				TEXT("reference remains a canonical object"),
				false)
				&& bValid;
		}
		return bValid;
	}

	bool VerifyConfigurationDefault(
		FAutomationTestBase* Test,
		const FUeAgentDefaultValue& Default,
		const FOwnedGraph& Graph)
	{
		bool bValid = Test->TestEqual(
			TEXT("configuration default source is retained"),
			Default.GetSourceKind(),
			EUeAgentDefaultSourceKind::ClassDefaultObject);
		bValid = Test->TestEqual(
			TEXT("configuration default source identity is retained"),
			Default.GetSourceIdentity(),
			Graph.Owner->GetPathName())
			&& bValid;
		bValid = Test->TestEqual(
			TEXT("configuration default provenance is retained"),
			Default.GetProvenance(),
			FString(TEXT("fixture-effective-default")))
			&& bValid;
		const FUeAgentCanonicalValueRef& Value =
			Default.GetValue();
		bValid = Test->TestEqual(
			TEXT("configuration default remains an object"),
			Value->GetKind(),
			EUeAgentValueKind::Object)
			&& bValid;
		if (Value->GetKind() != EUeAgentValueKind::Object)
		{
			return false;
		}
		const FUeAgentCanonicalValueRef* Enabled =
			FindObjectField(Value, TEXT("enabled"));
		const FUeAgentCanonicalValueRef* ExactCount =
			FindObjectField(Value, TEXT("exact_count"));
		const FUeAgentCanonicalValueRef* Target =
			FindObjectField(Value, TEXT("target"));
		bValid = Test->TestNotNull(
			TEXT("default boolean remains present"),
			Enabled)
			&& bValid;
		bValid = Test->TestNotNull(
			TEXT("default integer remains present"),
			ExactCount)
			&& bValid;
		bValid = Test->TestNotNull(
			TEXT("default null reference remains present"),
			Target)
			&& bValid;
		if (Enabled)
		{
			bValid = Test->TestFalse(
				TEXT("default boolean differs from current"),
				(*Enabled)->GetBoolean())
				&& bValid;
		}
		if (ExactCount)
		{
			bValid = Test->TestEqual(
				TEXT("default integer lexeme is exact"),
				(*ExactCount)->GetLexeme(),
				FString(TEXT("0")))
				&& bValid;
		}
		if (Target)
		{
			bValid = Test->TestEqual(
				TEXT("default reference is canonical null"),
				(*Target)->GetKind(),
				EUeAgentValueKind::Null)
				&& bValid;
		}
		return bValid;
	}

	bool VerifyTypedPropertySnapshot(
		FAutomationTestBase* Test,
		const TSharedPtr<const FUeAgentCanonicalSnapshot>& Snapshot,
		const FOwnedGraph& Graph)
	{
		const FString Base =
			TEXT("content_graph.property:")
			+ Graph.ItemId
			+ TEXT(":configuration");
		const FUeAgentStateEntry* Entry =
			FindEntry(Snapshot, Base + TEXT(":value"));
		if (!Test->TestNotNull(
				TEXT("configuration state entry exists"),
				Entry))
		{
			return false;
		}
		bool bValid = Test->TestEqual(
			TEXT("configuration preserves field owner identity"),
			Entry->GetIdentity().GetOwnerId(),
			Graph.ItemId);
		bValid = Test->TestEqual(
			TEXT("configuration canonical type id"),
			Entry->GetType().GetTypeId(),
			FString(TEXT("fixture.content_graph.configuration")))
			&& bValid;
		bValid = Test->TestEqual(
			TEXT("configuration canonical type kind"),
			Entry->GetType().GetValueKind(),
			EUeAgentValueKind::Object)
			&& bValid;
		bValid = Test->TestEqual(
			TEXT("configuration canonical container kind"),
			Entry->GetType().GetContainerKind(),
			FString(TEXT("object")))
			&& bValid;
		bValid = Test->TestEqual(
			TEXT("configuration authored presence"),
			Entry->GetAuthoredPresence(),
			EUeAgentAuthoredPresence::Present)
			&& bValid;
		bValid = Test->TestEqual(
			TEXT("configuration override state"),
			Entry->GetOverrideState(),
			EUeAgentOverrideState::Overridden)
			&& bValid;
		bValid = VerifyConfigurationValue(
			Test,
			Entry->GetValue(),
			Graph)
			&& bValid;
		bValid = VerifyConfigurationDefault(
			Test,
			Entry->GetDefault(),
			Graph)
			&& bValid;
		bValid = Test->TestNull(
			TEXT(
				"default is not duplicated as a string state "
					"entry"),
			FindEntry(Snapshot, Base + TEXT(":default")))
			&& bValid;
		bValid = Test->TestNull(
			TEXT(
				"presence is not duplicated as a boolean state "
					"entry"),
			FindEntry(Snapshot, Base + TEXT(":presence")))
			&& bValid;
		return bValid;
	}

	bool VerifyTypedParameterSnapshot(
		FAutomationTestBase* Test,
		const TSharedPtr<const FUeAgentCanonicalSnapshot>& Snapshot,
		const FOwnedGraph& Graph)
	{
		const FString Base =
			TEXT("content_graph.parameter:parameter.")
			+ Graph.Domain;
		const FUeAgentStateEntry* Entry =
			FindEntry(Snapshot, Base + TEXT(":value"));
		if (!Test->TestNotNull(
				TEXT("typed parameter state entry exists"),
				Entry))
		{
			return false;
		}
		bool bValid = Test->TestEqual(
			TEXT("parameter preserves owner identity"),
			Entry->GetIdentity().GetOwnerId(),
			Graph.ItemId);
		bValid = Test->TestEqual(
			TEXT("parameter canonical type id"),
			Entry->GetType().GetTypeId(),
			FString(TEXT("fixture.content_graph.parameter_list")))
			&& bValid;
		bValid = Test->TestEqual(
			TEXT("parameter canonical type kind"),
			Entry->GetType().GetValueKind(),
			EUeAgentValueKind::Array)
			&& bValid;
		bValid = Test->TestEqual(
			TEXT("parameter authored presence"),
			Entry->GetAuthoredPresence(),
			EUeAgentAuthoredPresence::Present)
			&& bValid;
		bValid = Test->TestEqual(
			TEXT("parameter override state"),
			Entry->GetOverrideState(),
			EUeAgentOverrideState::Overridden)
			&& bValid;
		const FUeAgentCanonicalValueRef& Value = Entry->GetValue();
		bValid = Test->TestEqual(
			TEXT("parameter remains a canonical array"),
			Value->GetKind(),
			EUeAgentValueKind::Array)
			&& bValid;
		if (Value->GetKind() == EUeAgentValueKind::Array)
		{
			bValid = Test->TestEqual(
				TEXT("parameter array shape is retained"),
				Value->GetArrayItems().Num(),
				3)
				&& bValid;
			if (Value->GetArrayItems().Num() == 3)
			{
				bValid = Test->TestEqual(
					TEXT("parameter exact integer survives"),
					Value->GetArrayItems()[0]->GetLexeme(),
					FString(TEXT("9007199254740993")))
					&& bValid;
				bValid = Test->TestEqual(
					TEXT("parameter reference survives"),
					Value->GetArrayItems()[1]->GetKind(),
					EUeAgentValueKind::Object)
					&& bValid;
				bValid = Test->TestEqual(
					TEXT("parameter canonical null survives"),
					Value->GetArrayItems()[2]->GetKind(),
					EUeAgentValueKind::Null)
					&& bValid;
			}
		}
		const FUeAgentDefaultValue& Default = Entry->GetDefault();
		bValid = Test->TestEqual(
			TEXT("parameter default source is retained"),
			Default.GetSourceKind(),
			EUeAgentDefaultSourceKind::Schema)
			&& bValid;
		bValid = Test->TestEqual(
			TEXT("parameter default remains a canonical array"),
			Default.GetValue()->GetKind(),
			EUeAgentValueKind::Array)
			&& bValid;
		if (Default.GetValue()->GetKind()
			== EUeAgentValueKind::Array)
		{
			bValid = Test->TestEqual(
				TEXT("parameter default array is exact"),
				Default.GetValue()->GetArrayItems().Num(),
				0)
				&& bValid;
		}
		return bValid;
	}

	struct FProviderCase
	{
		FString Binding;
		FString Input;
		TArray<FString> ExpectedPrefixes;
	};

	TArray<FProviderCase> ProviderCases(const FOwnedGraph& Graph)
	{
		return {
			{
				TEXT("content_graph.read.summary"),
				TEXT("{\"include_counts\":true,"
					"\"include_revision\":true}"),
				{TEXT("content_graph.summary:")},
			},
			{
				TEXT("content_graph.read.structure"),
				TEXT("{\"include_hierarchy\":true,"
					"\"include_order\":true}"),
				{TEXT("content_graph.container:")},
			},
			{
				TEXT("content_graph.read.items"),
				TEXT("{}"),
				{TEXT("content_graph.item:")},
			},
			{
				TEXT("content_graph.read.relations"),
				TEXT("{}"),
				{TEXT("content_graph.relation:")},
			},
			{
				TEXT("content_graph.read.properties"),
				FString::Printf(
					TEXT("{\"field_ids\":[\"configuration\"],"
						"\"include_defaults\":true,"
						"\"include_presence\":true,"
						"\"owner_ids\":[\"%s\"]}"),
					*Graph.ItemId),
				{TEXT("content_graph.property:")},
			},
			{
				TEXT("content_graph.read.diagnostics"),
				TEXT("{\"include_resolved\":true,"
					"\"severity\":[\"warning\"]}"),
				{TEXT("content_graph.diagnostic:")},
			},
			{
				TEXT("content_graph.read.compound"),
				TEXT("{\"sections\":[\"summary\",\"structure\","
					"\"items\",\"relations\",\"properties\","
					"\"parameters\",\"diagnostics\"]}"),
				AllSemanticPrefixes,
			},
		};
	}

	bool VerifySelectedSections(
		FAutomationTestBase* Test,
		const FProviderCase& Case,
		const FOwnedGraph& Graph,
		const TSharedRef<FOwnedEnvironment>& Environment,
		const FUeAgentOperationContext& OperationContext)
	{
		const int32 CallsBefore = Environment->Calls;
		TSharedPtr<const FUeAgentCanonicalSnapshot> Snapshot;
		FUeAgentContentGraphReadError Error;
		if (!Test->TestTrue(
				*FString::Printf(
					TEXT("%s captures selected facts"),
					*Case.Binding),
				Capture(
					Test,
					Case.Binding,
					Case.Input,
					Target(Graph),
					Environment,
					OperationContext,
					Snapshot,
					Error)))
		{
			return false;
		}
		bool bValid = Test->TestEqual(
			*FString::Printf(
				TEXT("%s performs one native capture"),
				*Case.Binding),
			Environment->Calls,
			CallsBefore + 1);
		bValid = Test->TestTrue(
			*FString::Printf(
				TEXT("%s retains real graph owner"),
				*Case.Binding),
			HasOwner(Snapshot, Graph.Owner->GetPathName()))
			&& bValid;

		const TSet<FString> Expected(Case.ExpectedPrefixes);
		for (const FString& Prefix : AllSemanticPrefixes)
		{
			if (Expected.Contains(Prefix))
			{
				bValid = Test->TestTrue(
					*FString::Printf(
						TEXT("%s includes %s"),
						*Case.Binding,
						*Prefix),
					HasSemanticPrefix(Snapshot, Prefix))
					&& bValid;
			}
			else
			{
				bValid = Test->TestFalse(
					*FString::Printf(
						TEXT("%s excludes %s"),
						*Case.Binding,
						*Prefix),
					HasSemanticPrefix(Snapshot, Prefix))
					&& bValid;
			}
		}
		if (Case.Binding == TEXT("content_graph.read.properties")
			|| Case.Binding == TEXT("content_graph.read.compound"))
		{
			bValid = VerifyTypedPropertySnapshot(
				Test,
				Snapshot,
				Graph)
				&& bValid;
		}
		if (Case.Binding == TEXT("content_graph.read.compound"))
		{
			bValid = VerifyTypedParameterSnapshot(
				Test,
				Snapshot,
				Graph)
				&& bValid;
		}
		return bValid;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentContentGraphReadNativeOwnedFactsTest,
	"UeAgentInterface.DomainAdapters.ContentGraphRead.E2.Native."
		"SixDomainsAndSevenProviders",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentContentGraphReadNativeOwnedFactsTest::RunTest(
	const FString& Parameters)
{
	FOwnedContentGraphs Owner;
	if (!TestNotNull(TEXT("owned package"), Owner.Package)
		|| !TestEqual(TEXT("six owned domains"), Owner.Graphs.Num(), 6))
	{
		return false;
	}
	const FUeAgentResourceTrackerOwner ResourceTracker =
		Resources();
	const TSharedPtr<FUeAgentOperationContext> OperationContext =
		Context(this, ResourceTracker);
	if (!OperationContext.IsValid())
	{
		return false;
	}
	const TSharedRef<FOwnedEnvironment> Environment =
		MakeShared<FOwnedEnvironment>(Owner);

	bool bValid = VerifyCaptureStateContract(this);
	const FOwnedGraph& Blueprint = Owner.Graphs[0];
	for (const FProviderCase& Case : ProviderCases(Blueprint))
	{
		bValid = VerifySelectedSections(
			this,
			Case,
			Blueprint,
			Environment,
			*OperationContext)
			&& bValid;
	}

	const FProviderCase CompoundCase =
		ProviderCases(Blueprint).Last();
	for (int32 Index = 1; Index < Owner.Graphs.Num(); ++Index)
	{
		bValid = VerifySelectedSections(
			this,
			CompoundCase,
			Owner.Graphs[Index],
			Environment,
			*OperationContext)
			&& bValid;
	}

	TSharedPtr<IUeAgentContentGraphReadHook> CompoundHook;
	FUeAgentContentGraphReadError Error;
	TestTrue(
		TEXT("compound hook for negative paths"),
		FUeAgentContentGraphReadNativeHookFactory::TryCreate(
			TEXT("content_graph.read.compound"),
			Environment,
			CompoundHook,
			Error));
	const int32 CallsBeforeMissingTarget = Environment->Calls;
	TSharedPtr<const FUeAgentCanonicalSnapshot> Snapshot;
	TestFalse(
		TEXT("missing target fails before environment capture"),
		CompoundHook.IsValid()
			&& CompoundHook->TryCapture(
				FUeAgentContractValue(CompoundCase.Input),
				{},
				*OperationContext,
				Snapshot,
				Error));
	TestEqual(
		TEXT("missing target does not call environment"),
		Environment->Calls,
		CallsBeforeMissingTarget);
	TestEqual(
		TEXT("missing target error is explicit"),
		Error.Code,
		FString(TEXT("content_graph_read_target_required")));

	struct FInvalidStateCase
	{
		EInvalidFieldStateCase Fixture;
		FString Label;
		FString ErrorCode;
		FString SubjectId;
	};
	const TArray<FInvalidStateCase> InvalidStateCases = {
		{
			EInvalidFieldStateCase::MissingType,
			TEXT("missing canonical type"),
			TEXT("content_graph_read_state_type_missing"),
			TEXT("item.blueprint:configuration"),
		},
		{
			EInvalidFieldStateCase::MissingValue,
			TEXT("missing canonical value"),
			TEXT("content_graph_read_state_value_missing"),
			TEXT("item.blueprint:configuration"),
		},
		{
			EInvalidFieldStateCase::MissingDefault,
			TEXT("missing canonical default"),
			TEXT("content_graph_read_state_default_missing"),
			TEXT("item.blueprint:configuration"),
		},
		{
			EInvalidFieldStateCase::TypeMismatch,
			TEXT("canonical type mismatch"),
			TEXT("content_graph_read_state_type_mismatch"),
			TEXT("item.blueprint:configuration"),
		},
		{
			EInvalidFieldStateCase::
				OverrideWithoutAuthoredPresence,
			TEXT("override without authored presence"),
			TEXT("content_graph_read_state_presence_invalid"),
			TEXT("item.blueprint:configuration"),
		},
		{
			EInvalidFieldStateCase::ParameterMissingDefault,
			TEXT("parameter missing canonical default"),
			TEXT("content_graph_read_state_default_missing"),
			TEXT("parameter.blueprint"),
		},
	};
	for (const FInvalidStateCase& InvalidCase : InvalidStateCases)
	{
		Environment->InvalidFieldState = InvalidCase.Fixture;
		const int32 CallsBeforeInvalidState = Environment->Calls;
		Snapshot.Reset();
		Error = {};
		TestFalse(
			*FString::Printf(
				TEXT("%s is rejected"),
				*InvalidCase.Label),
			CompoundHook->TryCapture(
				FUeAgentContractValue(CompoundCase.Input),
				Target(Blueprint),
				*OperationContext,
				Snapshot,
				Error));
		TestEqual(
			*FString::Printf(
				TEXT("%s performs one capture"),
				*InvalidCase.Label),
			Environment->Calls,
			CallsBeforeInvalidState + 1);
		TestFalse(
			*FString::Printf(
				TEXT("%s exposes no partial snapshot"),
				*InvalidCase.Label),
			Snapshot.IsValid());
		TestEqual(
			*FString::Printf(
				TEXT("%s error is exact"),
				*InvalidCase.Label),
			Error.Code,
			InvalidCase.ErrorCode);
		TestEqual(
			*FString::Printf(
				TEXT("%s subject is exact"),
				*InvalidCase.Label),
			Error.SubjectId,
			InvalidCase.SubjectId);
	}
	Environment->InvalidFieldState =
		EInvalidFieldStateCase::None;
	Environment->bFail = true;
	const int32 CallsBeforeFailure = Environment->Calls;
	Snapshot.Reset();
	TestFalse(
		TEXT("capture error is not silent"),
		CompoundHook->TryCapture(
			FUeAgentContractValue(CompoundCase.Input),
			Target(Blueprint),
			*OperationContext,
			Snapshot,
			Error));
	TestEqual(
		TEXT("failed capture is not retried"),
		Environment->Calls,
		CallsBeforeFailure + 1);
	TestFalse(TEXT("failed capture exposes no snapshot"), Snapshot.IsValid());
	TestEqual(
		TEXT("environment error is preserved"),
		Error.Code,
		FString(TEXT("fixture_content_graph_capture_failed")));

	TestFalse(TEXT("owned package remains clean"), Owner.Package->IsDirty());
	for (const FOwnedGraph& Graph : Owner.Graphs)
	{
		TestTrue(
			*FString::Printf(TEXT("%s owner remains valid"), *Graph.Domain),
			IsValid(Graph.Owner));
		TestTrue(
			*FString::Printf(TEXT("%s item remains valid"), *Graph.Domain),
			IsValid(Graph.ItemObject));
	}
	return bValid;
}

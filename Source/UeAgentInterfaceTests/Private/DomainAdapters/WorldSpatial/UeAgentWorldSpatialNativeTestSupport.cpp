#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialNativeTestSupport.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include "Core/BindingGateway/Operation/UeAgentOperationBinding.h"
#include "Core/BindingGateway/Query/UeAgentDomainQuery.h"
#include "Core/BindingGateway/Query/UeAgentQueryBindingError.h"
#include "Core/ContextResource/UeAgentOperationContext.h"
#include "Core/Contract/UeAgentContractSchema.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "Core/ExecutionMutation/UeAgentExecutionMutation.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Core/Projection/UeAgentProjection.h"
#include "Core/Verification/UeAgentVerification.h"
#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialDescriptors.h"
#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialExecutionChains.h"
#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialProductionRuntime.h"
#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialRequests.h"

namespace UE::UAI::Tests::WorldSpatial
{
	namespace
	{
		FString FixturePath(const TCHAR* Name)
		{
			return FPaths::ConvertRelativePathToFull(FPaths::Combine(
				FPaths::ProjectPluginsDir(),
				TEXT("UeAgentInterface/Source/UeAgentInterfaceTests/Private/"
					"DomainAdapters/WorldSpatial/Fixtures"),
				Name));
		}

		TSharedPtr<FJsonObject> LoadFixture(
			FAutomationTestBase& Test,
			const TCHAR* Name)
		{
			FString Text;
			if (!Test.TestTrue(
					FString::Printf(TEXT("load %s"), Name),
					FFileHelper::LoadFileToString(Text, *FixturePath(Name))))
			{
				return nullptr;
			}
			TSharedPtr<FJsonObject> Object;
			const TSharedRef<TJsonReader<>> Reader =
				TJsonReaderFactory<>::Create(Text);
			Test.TestTrue(
				FString::Printf(TEXT("parse %s"), Name),
				FJsonSerializer::Deserialize(Reader, Object)
					&& Object.IsValid());
			return Object;
		}

		FString JsonObjectText(const TSharedPtr<FJsonObject>& Object)
		{
			FString Text;
			const TSharedRef<TJsonWriter<>> Writer =
				TJsonWriterFactory<>::Create(&Text);
			return Object.IsValid()
				&& FJsonSerializer::Serialize(Object.ToSharedRef(), Writer)
					? Text
					: FString();
		}

		void RewriteIdentityFramesForNativeExecution(
			const TSharedPtr<FJsonObject>& Root)
		{
			TFunction<void(TSharedPtr<FJsonValue>&)> RewriteValue;
			RewriteValue = [&](TSharedPtr<FJsonValue>& Value)
			{
				if (!Value.IsValid())
				{
					return;
				}
				if (Value->Type == EJson::Array)
				{
					TArray<TSharedPtr<FJsonValue>> Items = Value->AsArray();
					for (TSharedPtr<FJsonValue>& Item : Items)
					{
						RewriteValue(Item);
					}
					Value = MakeShared<FJsonValueArray>(MoveTemp(Items));
					return;
				}
				if (Value->Type != EJson::Object)
				{
					return;
				}
				const TSharedPtr<FJsonObject> Object = Value->AsObject();
				for (TPair<FString, TSharedPtr<FJsonValue>>& Field : Object->Values)
				{
					RewriteValue(Field.Value);
				}
				FString FrameId;
				if (!Object->TryGetStringField(TEXT("frame_id"), FrameId))
				{
					return;
				}
				const TMap<FString, FString> NativeFrames{
					{TEXT("graph.canvas.canonical"),
						TEXT("graph.editor.view.local")},
					{TEXT("graph.local.canonical"),
						TEXT("graph.node.integer.local")},
					{TEXT("fixture.subject.semantic"), TEXT("unreal.world")},
				};
				if (const FString* NativeFrame = NativeFrames.Find(FrameId))
				{
					Object->SetStringField(TEXT("frame_id"), *NativeFrame);
					Object->SetStringField(TEXT("basis_frame_id"), *NativeFrame);
					if (Object->HasField(TEXT("conversion_evidence_ids")))
					{
						Object->SetArrayField(
							TEXT("conversion_evidence_ids"),
							TArray<TSharedPtr<FJsonValue>>());
					}
				}
			};
			if (Root.IsValid())
			{
				for (TPair<FString, TSharedPtr<FJsonValue>>& Field : Root->Values)
				{
					RewriteValue(Field.Value);
				}
			}
		}
	}

	bool RequireNativeExecution(
		FAutomationTestBase& Test,
		const FString& ScenarioId)
	{
		return true;
	}

	using namespace UE::UAI::DomainAdapters::WorldSpatial;

	bool FOwnedNativeEnvironmentBase::TryConvertToNative(
		const FWorldSpatialValue& Value,
		const FString& NativeFrameId,
		const FString& NativeUnit,
		FUeAgentWorldSpatialCompiledValue& OutValue,
		FUeAgentWorldSpatialExecutionError& OutError)
	{
		OutValue = {};
		OutError = {};
		if (!Value.IsValid() || NativeFrameId.IsEmpty() || NativeUnit.IsEmpty())
		{
			OutError.Code = TEXT("owned_fixture_spatial_contract_invalid");
			OutError.SubjectId = Value.Subject;
			return false;
		}
		if (Value.Frame.FrameId != NativeFrameId
			|| Value.Frame.Unit != NativeUnit)
		{
			OutError.Code = TEXT("owned_fixture_conversion_path_unavailable");
			OutError.SubjectId = Value.Subject;
			return false;
		}
		TArray<double> Components;
		Components.Reserve(Value.Components.Num());
		for (const FString& Lexeme : Value.Components)
		{
			double Parsed = 0.0;
			if (!LexTryParseString(Parsed, *Lexeme))
			{
				OutError.Code = TEXT("owned_fixture_spatial_number_invalid");
				OutError.SubjectId = Value.Subject;
				return false;
			}
			Components.Add(Parsed);
		}
		OutValue = FUeAgentWorldSpatialCompiledValue{
			Value.Subject,
			Value.Quantity,
			NativeFrameId,
			Value.Frame.Dimension,
			NativeUnit,
			MoveTemp(Components),
			TEXT("owned_fixture_native_identity"),
		};
		return OutValue.IsValid();
	}

	bool FOwnedNativeEnvironmentBase::TryConvertFromNative(
		const FUeAgentWorldSpatialCompiledValue& Value,
		const FWorldSpatialFrame& SemanticFrame,
		FWorldSpatialValue& OutValue,
		FUeAgentWorldSpatialExecutionError& OutError)
	{
		OutValue = {};
		OutError = {};
		if (!Value.IsValid() || !SemanticFrame.IsValid()
			|| Value.Dimension != SemanticFrame.Dimension
			|| Value.FrameId != SemanticFrame.FrameId
			|| Value.Unit != SemanticFrame.Unit)
		{
			OutError.Code = TEXT("owned_fixture_readback_conversion_invalid");
			OutError.SubjectId = Value.Subject;
			return false;
		}
		TArray<FString> Components;
		Components.Reserve(Value.Components.Num());
		for (const double Component : Value.Components)
		{
			Components.Add(FString::SanitizeFloat(Component, 9));
		}
		OutValue.Subject = Value.Subject;
		OutValue.Quantity = Value.Quantity;
		OutValue.Frame = SemanticFrame;
		OutValue.Components = MoveTemp(Components);
		return OutValue.IsValid();
	}

	namespace
	{
		using namespace UE::UAI::BindingGateway::Operation;
		using namespace UE::UAI::BindingGateway::Query;
		using namespace UE::UAI::ContextResource;
		using namespace UE::UAI::Contract;
		using namespace UE::UAI::Discovery;
		using namespace UE::UAI::DomainAdapters::WorldSpatial;
		using namespace UE::UAI::ExecutionMutation;
		using namespace UE::UAI::PlanningPreflight;
		using namespace UE::UAI::Projection;
		using namespace UE::UAI::Verification;

		FUeAgentResourceTrackerOwner MakeResources()
		{
			return MakeShared<FUeAgentResourceTracker, ESPMode::ThreadSafe>(
				FUeAgentResourceBudget({
					{EUeAgentResourceDimension::InputBytes, 4 * 1024 * 1024},
					{EUeAgentResourceDimension::OutputBytes, 4 * 1024 * 1024},
					{EUeAgentResourceDimension::Nodes, 100000},
					{EUeAgentResourceDimension::Items, 100000},
					{EUeAgentResourceDimension::StringBytes, 4 * 1024 * 1024},
					{EUeAgentResourceDimension::WorkUnits, 100000},
					{
						EUeAgentResourceDimension::EstimatedMemoryBytes,
						16 * 1024 * 1024,
					},
				}));
		}

		const FUeAgentOperationDescriptor* FindCapability(
			const FUeAgentDescriptorRegistry& Registry,
			const FString& CapabilityId)
		{
			return Registry.GetDescriptors().FindByPredicate(
				[&CapabilityId](const FUeAgentOperationDescriptor& Descriptor)
				{
					return Descriptor.GetCapabilityId() == CapabilityId;
				});
		}

		const FUeAgentWorldSpatialExecutionChainContract* FindContract(
			const FString& CapabilityId)
		{
			return GetWorldSpatialExecutionChainContracts().FindByPredicate(
				[&CapabilityId](
					const FUeAgentWorldSpatialExecutionChainContract& Contract)
				{
					return Contract.GetCapabilityId() == CapabilityId;
				});
		}

		TSharedPtr<FJsonObject> FindTypedInput(
			const TSharedPtr<FJsonObject>& Fixture,
			const FString& CapabilityId)
		{
			if (!Fixture.IsValid())
			{
				return nullptr;
			}
			for (const TSharedPtr<FJsonValue>& Value
				: Fixture->GetArrayField(TEXT("cases")))
			{
				const TSharedPtr<FJsonObject> Case = Value->AsObject();
				if (Case.IsValid()
					&& Case->GetStringField(TEXT("capability_id"))
						== CapabilityId)
				{
					return Case->GetObjectField(TEXT("valid_input"));
				}
			}
			return nullptr;
		}

		TArray<FString> FindScenarioCapabilities(
			const TSharedPtr<FJsonObject>& Fixture,
			const FString& ScenarioId)
		{
			TArray<FString> Result;
			if (!Fixture.IsValid())
			{
				return Result;
			}
			for (const TSharedPtr<FJsonValue>& Value
				: Fixture->GetArrayField(TEXT("scenarios")))
			{
				const TSharedPtr<FJsonObject> Scenario = Value->AsObject();
				if (!Scenario.IsValid()
					|| Scenario->GetStringField(TEXT("scenario_id"))
						!= ScenarioId)
				{
					continue;
				}
				for (const TSharedPtr<FJsonValue>& Capability
					: Scenario->GetArrayField(TEXT("capabilities")))
				{
					Result.Add(Capability->AsString());
				}
				break;
			}
			return Result;
		}

		TSharedPtr<FUeAgentOperationContext> MakeContext(
			FAutomationTestBase& Test,
			const FUeAgentOperationDescriptor& Descriptor,
			const EUeAgentRequestKind RequestKind,
			const FString& RequestId,
			const FOwnedScenarioContext& Scenario,
			const FUeAgentResourceTrackerOwner& Resources)
		{
			const TArray<EUeAgentStatePlane>& Planes =
				Descriptor.GetExecutionContextPolicy().GetAllowedStatePlanes();
			if (!Test.TestTrue(
					TEXT("descriptor has an allowed state plane"),
					!Planes.IsEmpty()))
			{
				return nullptr;
			}

			FUeAgentExecutionContextInput Input;
			Input.ProjectInstanceId = TEXT("world-spatial-owned-project");
			Input.EditorInstanceId = TEXT("world-spatial-owned-editor");
			Input.StatePlane = Planes[0];
			Input.WorldType = EUeAgentExecutionWorldType::Preview;
			Input.WorldIdentity = Scenario.WorldIdentity;
			Input.AssetEditorIdentity = Scenario.AssetEditorIdentity;
			Input.ViewportIdentity = Scenario.ViewportIdentity;
			Input.SelectionIdentities = Scenario.SelectionIdentities;
			Input.ContextRevision = TEXT("world-spatial-owned-context-1");
			Input.bResolvedFromCurrent = false;

			FUeAgentExecutionContextRefPtr ExecutionContext;
			FUeAgentExecutionContextError ContextError;
			if (!Test.TestTrue(
					TEXT("freeze explicit owned scenario context"),
					FUeAgentExecutionContextRef::TryCreate(
						Input,
						Descriptor.GetIdentity(),
						RequestKind,
						Descriptor.GetExecutionContextPolicy(),
						ExecutionContext,
						ContextError)))
			{
				return nullptr;
			}

			const FUeAgentCallerContext Caller(
				TEXT("world-spatial-owned-fixture"),
				{TEXT("world-spatial:read"), TEXT("world-spatial:write")},
				TEXT("corr-") + RequestId);
			const FUeAgentOperationDeadline Deadline =
				FUeAgentOperationDeadline::FromTimeout(
					30.0,
					[]() { return 100.0; });
			const FUeAgentCancellationSource Cancellation(
				[]() { return 100.0; });
			TSharedPtr<FUeAgentOperationContext> Context;
			FUeAgentOperationContextError Error;
			if (RequestKind == EUeAgentRequestKind::Query)
			{
				const FUeAgentQueryRequest Request(
					Descriptor.GetIdentity(),
					RequestId,
					FUeAgentContractValue(TEXT("{}")));
				Test.TestTrue(
					TEXT("create owned query context"),
					FUeAgentOperationContext::TryCreate(
						Request,
						Caller,
						ExecutionContext.ToSharedRef(),
						Resources,
						Deadline,
						Cancellation.GetToken(),
						Context,
						Error));
			}
			else
			{
				const FUeAgentOperationRequest Request(
					Descriptor.GetIdentity(),
					RequestId,
					TEXT("idem-") + RequestId,
					FUeAgentContractValue(TEXT("{}")));
				Test.TestTrue(
					TEXT("create owned operation context"),
					FUeAgentOperationContext::TryCreate(
						Request,
						Caller,
						ExecutionContext.ToSharedRef(),
						Resources,
						Deadline,
						Cancellation.GetToken(),
						Context,
						Error));
			}
			return Context;
		}

		TOptional<FUeAgentCandidateBinding> MakeQueryTarget(
			const FUeAgentOperationDescriptor& Descriptor,
			const FString& CapabilityId,
			const FString& PublicRef)
		{
			if (PublicRef.IsEmpty())
			{
				return {};
			}
			return FUeAgentCandidateBinding(
				TEXT("owned-target:") + CapabilityId,
				FUeAgentCandidateSummary(
					FUeAgentCanonicalIdentity(
						TEXT("world-spatial-target:") + CapabilityId,
						TEXT("owned_fixture_target"),
						TEXT("owned-world"),
						PublicRef,
						TEXT("target"),
						TEXT("owned-generation")),
					Descriptor.GetIdentity(),
					TEXT("world_spatial_target"),
					PublicRef,
					TEXT("owned WorldSpatial target"),
					{TEXT("WorldSpatial"), TEXT("Fixture")}),
				FUeAgentCatalogRevision(
					TEXT("owned-world-spatial-catalog"),
					TEXT("owned-generation"),
					1),
				PublicRef,
				TEXT("world-spatial-owned"),
				{EUeAgentCandidateOperation::Read},
				TEXT("world-spatial-native-e2"));
		}

		bool RunScenarioThroughC0C3Impl(
			FAutomationTestBase& Test,
			const FString& ScenarioId,
			const TArray<FString>& Capabilities,
			const TSharedPtr<FJsonObject>& TypedInputs,
			const FUeAgentDescriptorRegistry& Descriptors,
			const FUeAgentDomainQueryProviderRegistry& QueryProviders,
			const TArray<TSharedRef<FUeAgentWorldSpatialOperationChain>>&
				OperationChains,
			const FOwnedScenarioContext& ScenarioContext,
			const TMap<FString, FString>& QueryTargetRefs,
			const FPrepareCapabilityInput& PrepareCapabilityInput,
			const FAfterCapability& AfterCapability,
			const FBeforeCapability& BeforeCapability,
			const TMap<FString, FString>& ExpectedOperationFailures,
			const FAfterQueryReadback& AfterQueryReadback)
		{
			const FUeAgentResourceTrackerOwner Resources = MakeResources();
			for (int32 Index = 0; Index < Capabilities.Num(); ++Index)
			{
				const FString& CapabilityId = Capabilities[Index];
				if (BeforeCapability)
				{
					BeforeCapability(CapabilityId);
				}
				const FUeAgentWorldSpatialExecutionChainContract* Contract =
					FindContract(CapabilityId);
				const FUeAgentOperationDescriptor* Descriptor =
					FindCapability(Descriptors, CapabilityId);
				const TSharedPtr<FJsonObject> RawInput =
					FindTypedInput(TypedInputs, CapabilityId);
				if (!Test.TestNotNull(TEXT("exact execution contract exists"), Contract)
					|| !Test.TestNotNull(TEXT("exact descriptor exists"), Descriptor)
					|| !Test.TestTrue(TEXT("typed input fixture exists"), RawInput.IsValid()))
				{
					continue;
				}
				if (PrepareCapabilityInput
					&& !PrepareCapabilityInput(Test, CapabilityId, RawInput))
				{
					Test.AddError(
						TEXT("world_spatial_input_preparation_failed:") + CapabilityId);
					continue;
				}

				const FUeAgentContractSchema* InputSchema =
					Descriptors.FindCompiledInputSchemaExact(Descriptor->GetIdentity());
				if (!Test.TestNotNull(TEXT("compiled C0 input schema exists"), InputSchema))
				{
					continue;
				}
				TOptional<FUeAgentContractValue> CanonicalInput;
				FUeAgentContractSchemaError SchemaError;
				if (!Test.TestTrue(
						TEXT("C0 canonicalizes scenario input: ") + CapabilityId,
						InputSchema->TryValidateAndCanonicalize(
							FUeAgentContractValue(JsonObjectText(RawInput)),
							[](const int64, const FString&,
								FUeAgentContractSchemaError&) { return true; },
							CanonicalInput,
							SchemaError))
					|| !CanonicalInput.IsSet())
				{
					continue;
				}

				const bool bQuery = Contract->GetC3Lane() == TEXT("c3q");
				const TSharedPtr<FUeAgentOperationContext> Context = MakeContext(
					Test,
					*Descriptor,
					bQuery ? EUeAgentRequestKind::Query
						: EUeAgentRequestKind::Operation,
					ScenarioId + TEXT("-") + FString::FromInt(Index),
					ScenarioContext,
					Resources);
				if (!Context.IsValid())
				{
					continue;
				}

				if (bQuery)
				{
					const TOptional<FUeAgentQueryDescriptor> QueryDescriptor =
						Descriptors.FindQueryExact(Descriptor->GetIdentity());
					if (!Test.TestTrue(TEXT("exact query descriptor exists"),
							QueryDescriptor.IsSet()))
					{
						continue;
					}
					const TSharedPtr<IUeAgentDomainQueryProvider> Provider =
						QueryProviders.FindExact(QueryDescriptor->GetProviderBinding());
					if (!Test.TestTrue(TEXT("exact c3q provider exists"), Provider.IsValid()))
					{
						continue;
					}
					TSharedPtr<const FUeAgentProjectionSourceValue> Source;
					FUeAgentQueryBindingError Error;
					const TOptional<FUeAgentCandidateBinding> Target = MakeQueryTarget(
						*Descriptor, CapabilityId, QueryTargetRefs.FindRef(CapabilityId));
					const bool bQueried = Provider->TryQuery(
						QueryDescriptor.GetValue(), CanonicalInput.GetValue(), Target,
						*Context, Source, Error);
					if (!Test.TestTrue(TEXT("c3q reads the exact scenario: ") + CapabilityId,
							bQueried)
						|| !Test.TestTrue(TEXT("c3q returns an independent c4 snapshot"),
							Source.IsValid() && Source->GetSnapshot().IsValid()))
					{
						if (!bQueried)
						{
							Test.AddError(TEXT("world_spatial_query_failed:")
								+ CapabilityId + TEXT(":") + Error.Code + TEXT(":")
								+ Error.SubjectId + TEXT(":") + Error.Detail);
						}
						continue;
					}
					if (AfterQueryReadback)
					{
						AfterQueryReadback(Test, CapabilityId, *Source->GetSnapshot());
					}
				}
				else
				{
					const TSharedRef<FUeAgentWorldSpatialOperationChain>* Chain =
						OperationChains.FindByPredicate(
							[&CapabilityId](
								const TSharedRef<FUeAgentWorldSpatialOperationChain>& Item)
							{
								return Item->GetContract().GetCapabilityId() == CapabilityId;
							});
					if (!Test.TestNotNull(TEXT("exact c3o chain exists"), Chain))
					{
						continue;
					}
					TOptional<FPlanningRequest> PlanningRequest;
					FUeAgentOperationBindingError Error;
					const bool bBuiltRequest = (*Chain)->TryBuildPlanningRequest(
						CanonicalInput.GetValue(), {}, *Context, PlanningRequest, Error);
					const FString ExpectedFailure =
						ExpectedOperationFailures.FindRef(CapabilityId);
					if (!ExpectedFailure.IsEmpty())
					{
						Test.TestFalse(TEXT("c3o rejects the protected scenario: ")
							+ CapabilityId, bBuiltRequest);
						Test.TestEqual(TEXT("c3o preserves the exact protection error: ")
							+ CapabilityId, Error.Code, ExpectedFailure);
						Test.TestFalse(TEXT("protected c3o creates no draft"),
							PlanningRequest.IsSet());
						continue;
					}
					if (!Test.TestTrue(TEXT("c3o creates typed c6 draft: ")
							+ CapabilityId, bBuiltRequest)
						|| !PlanningRequest.IsSet())
					{
						if (!bBuiltRequest)
						{
							Test.AddError(TEXT("world_spatial_operation_draft_failed:")
								+ CapabilityId + TEXT(":") + Error.Code + TEXT(":")
								+ Error.SubjectId + TEXT(":") + Error.Detail);
						}
						continue;
					}
					FPlanningResult Planning;
					FExecutionReceipt Receipt;
					FVerificationOutcome Outcome;
					const bool bPlanned = (*Chain)->GetPlanner().TryPlan(
						PlanningRequest.GetValue(), *Context, Planning, Error);
					const bool bExecuted = bPlanned
						&& (*Chain)->GetExecutor().TryExecute(
							Planning, {}, *Context, Receipt, Error);
					const bool bVerified = bExecuted
						&& (*Chain)->GetVerifier().TryVerify(
							Receipt, {}, *Context, Outcome, Error);
					if (!Test.TestTrue(TEXT("c6 plans the exact mutation"), bPlanned)
						|| !Test.TestTrue(TEXT("native mutation executes once"), bExecuted)
						|| !Test.TestTrue(
							TEXT("independent native readback verifies the effect"), bVerified))
					{
						Test.AddError(TEXT("world_spatial_operation_failed:")
							+ CapabilityId + TEXT(":") + Error.Code + TEXT(":")
							+ Error.SubjectId + TEXT(":") + Error.Detail);
						continue;
					}
					Test.TestEqual(TEXT("native effect is complete: ") + CapabilityId,
						Outcome.GetEffectOutcome(), EEffectOutcome::Complete);
					if (Outcome.GetEffectOutcome() != EEffectOutcome::Complete)
					{
						Test.AddError(TEXT("world_spatial_incomplete_evidence:")
							+ CapabilityId + TEXT(":")
							+ FString::Join(Outcome.GetEvidenceIds(), TEXT("|")));
					}
				}

				if (AfterCapability)
				{
					AfterCapability(Test, CapabilityId);
				}
			}
			return !Test.HasAnyErrors();
		}
	}

	bool RunOwnedScenarioThroughC0C3(
		FAutomationTestBase& Test,
		const FString& ScenarioId,
		const TSharedRef<FOwnedNativeEnvironmentBase>& Environment,
		const FOwnedScenarioContext& ScenarioContext,
		const TMap<FString, FString>& QueryTargetRefs,
		const FAfterCapability& AfterCapability,
		const FBeforeCapability& BeforeCapability,
		const TMap<FString, FString>& ExpectedOperationFailures,
		const FAfterQueryReadback& AfterQueryReadback)
	{
		const TSharedPtr<FJsonObject> Scenarios = LoadFixture(
			Test,
			TEXT("world_spatial_native_scenario_cases.json"));
		const TSharedPtr<FJsonObject> TypedInputs = LoadFixture(
			Test,
			TEXT("world_spatial_typed_request_cases.json"));
		const TArray<FString> Capabilities =
			FindScenarioCapabilities(Scenarios, ScenarioId);
		if (!Test.TestTrue(
				TEXT("scenario owns at least one exact capability"),
				!Capabilities.IsEmpty()))
		{
			return false;
		}

		TSharedPtr<const FUeAgentDescriptorRegistry> Descriptors;
		FString DescriptorError;
		if (!Test.TestTrue(
				TEXT("create production WorldSpatial descriptor registry"),
				TryCreateWorldSpatialDescriptorRegistry(
					Descriptors,
					DescriptorError))
			|| !Descriptors.IsValid())
		{
			Test.AddError(
				TEXT("world_spatial_descriptor_registry_unavailable:")
				+ DescriptorError);
			return false;
		}
		TSharedPtr<IUeAgentWorldSpatialRequestDecoder> Decoder;
		FUeAgentWorldSpatialExecutionError NativeError;
		if (!Test.TestTrue(
				TEXT("create production typed request decoder"),
				TryCreateWorldSpatialRequestDecoder(Decoder, NativeError))
			|| !Decoder.IsValid())
		{
			return false;
		}

		FUeAgentWorldSpatialNativePorts Ports;
		const FUeAgentWorldSpatialNativeDependencies Dependencies{
			StaticCastSharedRef<IUeAgentWorldSpatialNativeEnvironment>(Environment),
			StaticCastSharedRef<IUeAgentWorldSpatialSpaceCompiler>(Environment),
			StaticCastSharedRef<IUeAgentWorldSpatialRevisionSource>(Environment),
		};
		if (!Test.TestTrue(
				TEXT("create production native WorldSpatial ports"),
				TryCreateWorldSpatialNativePorts(
					Dependencies,
					Ports,
					NativeError))
			|| !Ports.IsValid())
		{
			return false;
		}
		TSharedPtr<const FUeAgentDomainQueryProviderRegistry> QueryProviders;
		if (!Test.TestTrue(
				TEXT("bind production C3Q to native readback"),
				TryCreateWorldSpatialQueryProviderRegistry(
					Decoder.ToSharedRef(),
					Ports.Readback.ToSharedRef(),
					QueryProviders,
					NativeError))
			|| !QueryProviders.IsValid())
		{
			return false;
		}
		TArray<TSharedRef<FUeAgentWorldSpatialOperationChain>> OperationChains;
		if (!Test.TestTrue(
				TEXT("bind production C3O to native mutation/readback"),
				TryCreateWorldSpatialOperationChains(
					Decoder.ToSharedRef(),
					Ports.Readback.ToSharedRef(),
					Ports.Mutation.ToSharedRef(),
					Ports.Settle.ToSharedRef(),
					Ports.Comparator.ToSharedRef(),
					OperationChains,
					NativeError)))
		{
			return false;
		}

		return RunScenarioThroughC0C3Impl(
			Test,
			ScenarioId,
			Capabilities,
			TypedInputs,
			*Descriptors,
			*QueryProviders,
			OperationChains,
			ScenarioContext,
			QueryTargetRefs,
			[](FAutomationTestBase&,
				const FString&,
				const TSharedPtr<FJsonObject>& RawInput)
			{
				RewriteIdentityFramesForNativeExecution(RawInput);
				return true;
			},
			AfterCapability,
			BeforeCapability,
			ExpectedOperationFailures,
			AfterQueryReadback);
	}

	bool TryCreateProductionRuntimeForTest(
		FAutomationTestBase& Test,
		UWorld& ExplicitWorld,
		TSharedPtr<FUeAgentWorldSpatialProductionRuntime>& OutRuntime)
	{
		FUeAgentWorldSpatialExecutionError Error;
		const FUeAgentWorldSpatialProductionRuntimeConfig Config{
			TEXT("world-spatial-production-test-project"),
			TEXT("world-spatial-production-test-editor"),
			TEXT("world-spatial-production-test-session"),
			1,
		};
		const bool bCreated = FUeAgentWorldSpatialProductionRuntime::TryCreate(
			Config, ExplicitWorld, OutRuntime, Error);
		if (!Test.TestTrue(TEXT("create exact production WorldSpatial runtime"),
				bCreated && OutRuntime.IsValid()))
		{
			Test.AddError(TEXT("world_spatial_production_runtime_failed:")
				+ Error.Code + TEXT(":") + Error.SubjectId + TEXT(":")
				+ Error.Detail);
			return false;
		}
		return true;
	}

	bool TryCreateProductionRuntimeForTest(
		FAutomationTestBase& Test,
		UWorld& ExplicitWorld,
		const FString& ViewportIdentity,
		FEditorViewportClient& ViewportClient,
		FViewport& Viewport,
		UGeometrySelectionManager* SelectionManager,
		const TArray<FUeAgentWorldSpatialMeshSelectionTarget>& SelectionTargets,
		TSharedPtr<FUeAgentWorldSpatialProductionRuntime>& OutRuntime)
	{
		FUeAgentWorldSpatialProductionRuntimeConfig Config{
			TEXT("world-spatial-production-test-project"),
			TEXT("world-spatial-production-test-editor"),
			TEXT("world-spatial-production-test-session"),
			1,
		};
		Config.ViewportBindings.Add(
			{ViewportIdentity, &ViewportClient, &Viewport});
		if (SelectionManager)
		{
			Config.MeshSelectionBindings.Add(
				{ViewportIdentity, SelectionManager, SelectionTargets});
		}
		FUeAgentWorldSpatialExecutionError Error;
		const bool bCreated = FUeAgentWorldSpatialProductionRuntime::TryCreate(
			Config, ExplicitWorld, OutRuntime, Error);
		if (!Test.TestTrue(TEXT("create explicitly bound production runtime"),
				bCreated && OutRuntime.IsValid()))
		{
			Test.AddError(TEXT("world_spatial_bound_runtime_failed:")
				+ Error.Code + TEXT(":") + Error.SubjectId + TEXT(":")
				+ Error.Detail);
			return false;
		}
		return true;
	}

	bool PrepareProductionCapabilityInput(
		FAutomationTestBase& Test,
		const FString& CapabilityId,
		const TSharedPtr<FJsonObject>& RawInput,
		const FUeAgentWorldSpatialProductionRuntime& Runtime,
		const FProductionScenarioBindings& Bindings)
	{
		if (!Test.TestTrue(TEXT("production input is mutable JSON"),
				RawInput.IsValid()))
		{
			return false;
		}

		TFunction<void(TSharedPtr<FJsonValue>&)> RewriteValue;
		RewriteValue = [&](TSharedPtr<FJsonValue>& Value)
		{
			if (!Value.IsValid())
			{
				return;
			}
			if (Value->Type == EJson::String)
			{
				const FString Original = Value->AsString();
				const FString* Replacement = Bindings.ExplicitRefs.Find(Original);
				if (Replacement)
				{
					Value = MakeShared<FJsonValueString>(*Replacement);
				}
				else if (Original == TEXT("viewport:fixture")
					&& !Bindings.ViewportIdentity.IsEmpty())
				{
					Value = MakeShared<FJsonValueString>(Bindings.ViewportIdentity);
				}
				return;
			}
			if (Value->Type == EJson::Array)
			{
				TArray<TSharedPtr<FJsonValue>> Items = Value->AsArray();
				for (TSharedPtr<FJsonValue>& Item : Items)
				{
					RewriteValue(Item);
				}
				Value = MakeShared<FJsonValueArray>(MoveTemp(Items));
				return;
			}
			if (Value->Type != EJson::Object)
			{
				return;
			}
			const TSharedPtr<FJsonObject> Object = Value->AsObject();
			for (TPair<FString, TSharedPtr<FJsonValue>>& Field : Object->Values)
			{
				RewriteValue(Field.Value);
			}
		};
		for (TPair<FString, TSharedPtr<FJsonValue>>& Field : RawInput->Values)
		{
			RewriteValue(Field.Value);
		}
		RewriteIdentityFramesForNativeExecution(RawInput);

		auto WriteRevision = [&](const TSharedPtr<FJsonObject>& Revision,
			const FString& ExplicitRef) -> bool
		{
			if (!Revision.IsValid() || ExplicitRef.IsEmpty())
			{
				Test.AddError(TEXT("production revision target is missing:")
					+ CapabilityId);
				return false;
			}
			FWorldSpatialRevision Actual;
			FUeAgentWorldSpatialExecutionError Error;
			if (!Runtime.TryReadRevision(ExplicitRef, Actual, Error))
			{
				Test.AddError(TEXT("production revision read failed:")
					+ CapabilityId + TEXT(":") + Error.Code + TEXT(":")
					+ Error.SubjectId + TEXT(":") + Error.Detail);
				return false;
			}
			Revision->SetStringField(TEXT("source_id"), Actual.SourceId);
			Revision->SetStringField(TEXT("generation"), Actual.Generation);
			Revision->SetNumberField(TEXT("sequence"), Actual.Sequence);
			return true;
		};

		if (CapabilityId == TEXT("graph.view.update"))
		{
			const TSharedPtr<FJsonObject> State =
				RawInput->GetObjectField(TEXT("view_state"));
			return WriteRevision(
				State->GetObjectField(TEXT("graph_revision")),
				State->GetObjectField(TEXT("location"))->GetStringField(TEXT("subject")));
		}
		if (CapabilityId == TEXT("graph.layout.apply"))
		{
			return WriteRevision(
				RawInput->GetObjectField(TEXT("expected_revision")),
				RawInput->GetStringField(TEXT("graph_ref")));
		}
		if (CapabilityId == TEXT("world.object_align")
			|| CapabilityId == TEXT("world.object_snap_to_surface"))
		{
			for (const TSharedPtr<FJsonValue>& Value
				: RawInput->GetArrayField(TEXT("expected_revisions")))
			{
				const TSharedPtr<FJsonObject> Entry = Value->AsObject();
				if (!WriteRevision(
						Entry->GetObjectField(TEXT("revision")),
						Entry->GetStringField(TEXT("target_ref"))))
				{
					return false;
				}
			}
		}
		else if (CapabilityId == TEXT("mesh.transform.bake"))
		{
			return WriteRevision(
				RawInput->GetObjectField(TEXT("expected_revision")),
				RawInput->GetObjectField(TEXT("transform"))
					->GetStringField(TEXT("subject")));
		}
		else if (CapabilityId == TEXT("mesh.pivot.update"))
		{
			return WriteRevision(
				RawInput->GetObjectField(TEXT("expected_revision")),
				RawInput->GetObjectField(TEXT("pivot"))
					->GetStringField(TEXT("subject")));
		}
		else if (CapabilityId == TEXT("viewport.state.restore"))
		{
			const TSharedPtr<FJsonObject> Snapshot =
				RawInput->GetObjectField(TEXT("snapshot"));
			return WriteRevision(
				Snapshot->GetObjectField(TEXT("observed_revision")),
				Snapshot->GetStringField(TEXT("viewport_ref")));
		}
		return true;
	}

	bool RunProductionScenarioThroughC0C3(
		FAutomationTestBase& Test,
		const FString& ScenarioId,
		const TSharedRef<FUeAgentWorldSpatialProductionRuntime>& Runtime,
		const FOwnedScenarioContext& ScenarioContext,
		const TMap<FString, FString>& QueryTargetRefs,
		const FPrepareCapabilityInput& PrepareCapabilityInput,
		const FAfterCapability& AfterCapability,
		const FBeforeCapability& BeforeCapability,
		const TMap<FString, FString>& ExpectedOperationFailures,
		const FAfterQueryReadback& AfterQueryReadback)
	{
		const TSharedPtr<FJsonObject> Scenarios = LoadFixture(
			Test, TEXT("world_spatial_native_scenario_cases.json"));
		const TSharedPtr<FJsonObject> TypedInputs = LoadFixture(
			Test, TEXT("world_spatial_typed_request_cases.json"));
		const TArray<FString> Capabilities =
			FindScenarioCapabilities(Scenarios, ScenarioId);
		if (!Test.TestTrue(TEXT("production scenario owns exact capabilities"),
				!Capabilities.IsEmpty())
			|| !Test.TestTrue(TEXT("production descriptor registry is valid"),
				Runtime->GetDescriptorRegistry().IsValid())
			|| !Test.TestTrue(TEXT("production query registry is valid"),
				Runtime->GetQueryProviderRegistry().IsValid()))
		{
			return false;
		}
		return RunScenarioThroughC0C3Impl(
			Test,
			ScenarioId,
			Capabilities,
			TypedInputs,
			*Runtime->GetDescriptorRegistry(),
			*Runtime->GetQueryProviderRegistry(),
			Runtime->GetOperationChains(),
			ScenarioContext,
			QueryTargetRefs,
			PrepareCapabilityInput,
			AfterCapability,
			BeforeCapability,
			ExpectedOperationFailures,
			AfterQueryReadback);
	}

	bool RunProductionCapabilitiesThroughC0C3(
		FAutomationTestBase& Test,
		const FString& ScenarioId,
		const TArray<FString>& Capabilities,
		const TSharedPtr<FJsonObject>& TypedInputs,
		const TSharedRef<FUeAgentWorldSpatialProductionRuntime>& Runtime,
		const FOwnedScenarioContext& ScenarioContext,
		const TMap<FString, FString>& QueryTargetRefs,
		const FPrepareCapabilityInput& PrepareCapabilityInput,
		const FAfterCapability& AfterCapability,
		const FBeforeCapability& BeforeCapability,
		const TMap<FString, FString>& ExpectedOperationFailures,
		const FAfterQueryReadback& AfterQueryReadback)
	{
		if (!Test.TestTrue(TEXT("production descriptor registry is valid"),
				Runtime->GetDescriptorRegistry().IsValid())
			|| !Test.TestTrue(TEXT("production query registry is valid"),
				Runtime->GetQueryProviderRegistry().IsValid())
			|| !Test.TestTrue(TEXT("custom typed inputs are valid"),
				TypedInputs.IsValid())
			|| !Test.TestTrue(TEXT("custom capability set is bounded"),
				!Capabilities.IsEmpty()))
		{
			return false;
		}
		return RunScenarioThroughC0C3Impl(
			Test,
			ScenarioId,
			Capabilities,
			TypedInputs,
			*Runtime->GetDescriptorRegistry(),
			*Runtime->GetQueryProviderRegistry(),
			Runtime->GetOperationChains(),
			ScenarioContext,
			QueryTargetRefs,
			PrepareCapabilityInput,
			AfterCapability,
			BeforeCapability,
			ExpectedOperationFailures,
			AfterQueryReadback);
	}
}

// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Niagara/UeAgentNiagaraRuntimeParticleCapture.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/SecureHash.h"
#include "NiagaraDataSet.h"
#include "NiagaraEmitterInstance.h"
#include "NiagaraTypes.h"

namespace UeAgentNiagaraRuntimeParticleCapture
{
	namespace
	{
		FString StableHash(const FString& Text)
		{
			return FString::Printf(TEXT("md5:%s"), *FMD5::HashAnsiString(*Text));
		}

		FString JoinStable(const TArray<FString>& Values)
		{
			return FString::Join(Values, TEXT("\x1f"));
		}

		TSharedRef<FJsonObject> Unavailable(
			const FOptions& Options,
			const FString& Reason,
			const FString& SimTarget)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> Requested;
			for (const FString& Name : Options.AttributeNames)
			{
				Requested.Add(MakeShared<FJsonValueString>(Name));
			}
			Result->SetArrayField(TEXT("requested_attributes"), Requested);
			Result->SetBoolField(TEXT("available"), false);
			Result->SetStringField(TEXT("reason"), Reason);
			Result->SetStringField(TEXT("sim_target"), SimTarget);
			Result->SetNumberField(TEXT("max_particle_samples"), Options.MaxParticleSamples);
			return Result;
		}
	}

	int32 ResolveAttributeNameIndex(const TArray<FString>& AvailableNames, const FString& RequestedName)
	{
		for (int32 Index = 0; Index < AvailableNames.Num(); ++Index)
		{
			if (AvailableNames[Index].Equals(RequestedName, ESearchCase::IgnoreCase)) return Index;
		}

		auto WithoutParticleNamespace = [](const FString& Name)
		{
			return Name.StartsWith(TEXT("Particles."), ESearchCase::IgnoreCase)
				? Name.RightChop(10)
				: Name;
		};
		const FString CanonicalRequestedName = WithoutParticleNamespace(RequestedName);
		int32 AliasMatch = INDEX_NONE;
		for (int32 Index = 0; Index < AvailableNames.Num(); ++Index)
		{
			if (!WithoutParticleNamespace(AvailableNames[Index]).Equals(
				CanonicalRequestedName,
				ESearchCase::IgnoreCase))
			{
				continue;
			}
			if (AliasMatch != INDEX_NONE) return INDEX_NONE;
			AliasMatch = Index;
		}
		return AliasMatch;
	}

	TSharedRef<FJsonObject> Capture(FNiagaraEmitterInstance& EmitterInstance, const FOptions& Options)
	{
		const FString SimTarget = EmitterInstance.GetSimTarget() == ENiagaraSimTarget::CPUSim
			? TEXT("CPUSim")
			: TEXT("GPUComputeSim");
		if (EmitterInstance.GetSimTarget() != ENiagaraSimTarget::CPUSim)
		{
			return Unavailable(Options, TEXT("gpu_readback_not_requested"), SimTarget);
		}
		if (EmitterInstance.AsStateful() == nullptr)
		{
			return Unavailable(Options, TEXT("stateless_emitter_has_no_cpu_particle_dataset"), SimTarget);
		}

		const FNiagaraDataBuffer* Data = EmitterInstance.GetParticleData().GetCurrentData();
		if (!Data)
		{
			return Unavailable(Options, TEXT("particle_dataset_current_buffer_unavailable"), SimTarget);
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Requested;
		for (const FString& Name : Options.AttributeNames)
		{
			Requested.Add(MakeShared<FJsonValueString>(Name));
		}
		Result->SetArrayField(TEXT("requested_attributes"), Requested);
		Result->SetBoolField(TEXT("available"), true);
		Result->SetStringField(TEXT("reason"), TEXT(""));
		Result->SetStringField(TEXT("sim_target"), SimTarget);
		Result->SetNumberField(TEXT("max_particle_samples"), Options.MaxParticleSamples);

		FNiagaraDataVariableIterator LayoutIterator(Data);
		const TArray<FNiagaraVariable>& LayoutVariables = LayoutIterator.GetVariables();
		TArray<FString> LayoutVariableNames;
		LayoutVariableNames.Reserve(LayoutVariables.Num());
		for (const FNiagaraVariable& Variable : LayoutVariables)
		{
			LayoutVariableNames.Add(Variable.GetName().ToString());
		}
		const int32 ParticleCount = static_cast<int32>(Data->GetNumInstances());
		const int32 SampleCount = FMath::Min(ParticleCount, Options.MaxParticleSamples);
		Result->SetNumberField(TEXT("particle_count"), ParticleCount);
		Result->SetNumberField(TEXT("sampled_particle_count"), SampleCount);
		Result->SetBoolField(TEXT("complete"), SampleCount == ParticleCount);
		Result->SetNumberField(TEXT("data_set_variable_count"), LayoutVariables.Num());

		struct FAttributeAccumulator
		{
			FString RequestedName;
			FString ResolvedName;
			FString TypeName;
			int32 VariableIndex = INDEX_NONE;
			TArray<FString> OrderedValueHashes;
			TSet<FString> DistinctValueHashes;
			TArray<TSharedPtr<FJsonValue>> Samples;
		};

		TArray<FAttributeAccumulator> Accumulators;
		Accumulators.Reserve(Options.AttributeNames.Num());
		bool bAllRequestedFound = true;
		for (const FString& RequestedName : Options.AttributeNames)
		{
			FAttributeAccumulator& Accumulator = Accumulators.AddDefaulted_GetRef();
			Accumulator.RequestedName = RequestedName;
			Accumulator.VariableIndex = ResolveAttributeNameIndex(LayoutVariableNames, RequestedName);
			if (Accumulator.VariableIndex == INDEX_NONE)
			{
				bAllRequestedFound = false;
				continue;
			}
			const FNiagaraVariable& Variable = LayoutVariables[Accumulator.VariableIndex];
			Accumulator.ResolvedName = Variable.GetName().ToString();
			Accumulator.TypeName = Variable.GetType().GetName();
		}

		TArray<FString> OrderedRowHashes;
		TArray<FString> SetRowHashes;
		OrderedRowHashes.Reserve(SampleCount);
		SetRowHashes.Reserve(SampleCount);
		for (int32 ParticleIndex = 0; ParticleIndex < SampleCount; ++ParticleIndex)
		{
			FNiagaraDataVariableIterator Iterator(Data, ParticleIndex);
			Iterator.Get();
			const TArray<FNiagaraVariable>& Variables = Iterator.GetVariables();
			TArray<FString> RowParts;
			RowParts.Reserve(Accumulators.Num());
			for (FAttributeAccumulator& Accumulator : Accumulators)
			{
				if (!Variables.IsValidIndex(Accumulator.VariableIndex))
				{
					RowParts.Add(Accumulator.RequestedName + TEXT("=<missing>"));
					continue;
				}
				const FNiagaraVariable& Variable = Variables[Accumulator.VariableIndex];
				const FString ValueText = Variable.GetType().ToString(Variable.GetData());
				const FString ValueHash = StableHash(ValueText);
				Accumulator.OrderedValueHashes.Add(ValueHash);
				Accumulator.DistinctValueHashes.Add(ValueHash);
				TSharedRef<FJsonObject> Sample = MakeShared<FJsonObject>();
				Sample->SetNumberField(TEXT("particle_index"), ParticleIndex);
				Sample->SetStringField(TEXT("value_text"), ValueText);
				Sample->SetStringField(TEXT("value_hash"), ValueHash);
				Accumulator.Samples.Add(MakeShared<FJsonValueObject>(Sample));
				RowParts.Add(Accumulator.ResolvedName + TEXT("=") + ValueHash);
			}
			const FString RowHash = StableHash(JoinStable(RowParts));
			OrderedRowHashes.Add(RowHash);
			SetRowHashes.Add(RowHash);
		}

		TArray<TSharedPtr<FJsonValue>> AttributeJson;
		for (const FAttributeAccumulator& Accumulator : Accumulators)
		{
			TSharedRef<FJsonObject> Attribute = MakeShared<FJsonObject>();
			Attribute->SetStringField(TEXT("requested_name"), Accumulator.RequestedName);
			const bool bFound = Accumulator.VariableIndex != INDEX_NONE;
			Attribute->SetBoolField(TEXT("found"), bFound);
			if (bFound)
			{
				Attribute->SetStringField(TEXT("resolved_name"), Accumulator.ResolvedName);
				Attribute->SetStringField(TEXT("type"), Accumulator.TypeName);
				Attribute->SetNumberField(TEXT("distinct_value_count"), Accumulator.DistinctValueHashes.Num());
				Attribute->SetStringField(TEXT("ordered_hash"), StableHash(JoinStable(Accumulator.OrderedValueHashes)));
				TArray<FString> SortedValueHashes = Accumulator.OrderedValueHashes;
				SortedValueHashes.Sort();
				Attribute->SetStringField(TEXT("set_hash"), StableHash(JoinStable(SortedValueHashes)));
				Attribute->SetArrayField(TEXT("samples"), Accumulator.Samples);
			}
			AttributeJson.Add(MakeShared<FJsonValueObject>(Attribute));
		}

		SetRowHashes.Sort();
		Result->SetBoolField(TEXT("all_requested_found"), bAllRequestedFound);
		Result->SetStringField(TEXT("particle_attribute_ordered_hash"), StableHash(JoinStable(OrderedRowHashes)));
		Result->SetStringField(TEXT("particle_attribute_set_hash"), StableHash(JoinStable(SetRowHashes)));
		Result->SetArrayField(TEXT("attributes"), AttributeJson);
		return Result;
	}
}

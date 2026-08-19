#include "DomainAdapters/TimelineAnimation/UeAgentTimelineAnimationContract.h"

namespace UE::UAI::DomainAdapters::TimelineAnimation
{
	FUeAgentTimelineAnimationPortFamilyContract::
	FUeAgentTimelineAnimationPortFamilyContract(
		FString InFamilyId,
		TArray<FString> InRequiredContextFacets,
		TArray<FString> InAcceptedTimeStates,
		FString InClockPolicy,
		FString InRangePolicy,
		FString InSpatialPolicy,
		FString InOracleKind)
		: FamilyId(MoveTemp(InFamilyId))
		, RequiredContextFacets(MoveTemp(InRequiredContextFacets))
		, AcceptedTimeStates(MoveTemp(InAcceptedTimeStates))
		, ClockPolicy(MoveTemp(InClockPolicy))
		, RangePolicy(MoveTemp(InRangePolicy))
		, SpatialPolicy(MoveTemp(InSpatialPolicy))
		, OracleKind(MoveTemp(InOracleKind))
	{
	}

	const FString& FUeAgentTimelineAnimationPortFamilyContract::GetFamilyId() const { return FamilyId; }
	const FString& FUeAgentTimelineAnimationPortFamilyContract::GetTemporalContractId() const { return TemporalContractId; }
	const FString& FUeAgentTimelineAnimationPortFamilyContract::GetSpaceContractId() const { return SpaceContractId; }
	const TArray<FString>& FUeAgentTimelineAnimationPortFamilyContract::GetRequiredContextFacets() const { return RequiredContextFacets; }
	const TArray<FString>& FUeAgentTimelineAnimationPortFamilyContract::GetAcceptedTimeStates() const { return AcceptedTimeStates; }
	const FString& FUeAgentTimelineAnimationPortFamilyContract::GetClockPolicy() const { return ClockPolicy; }
	const FString& FUeAgentTimelineAnimationPortFamilyContract::GetRangePolicy() const { return RangePolicy; }
	const FString& FUeAgentTimelineAnimationPortFamilyContract::GetSpatialPolicy() const { return SpatialPolicy; }
	const FString& FUeAgentTimelineAnimationPortFamilyContract::GetOracleKind() const { return OracleKind; }

	FUeAgentTimelineAnimationCapabilityContract::
	FUeAgentTimelineAnimationCapabilityContract(
		FString InCapabilityId,
		FString InDescriptorId,
		int32 InVersion,
		FString InOperationKind,
		FString InEffectClass,
		FString InPortFamilyId)
		: CapabilityId(MoveTemp(InCapabilityId))
		, DescriptorId(MoveTemp(InDescriptorId))
		, Version(InVersion)
		, OperationKind(MoveTemp(InOperationKind))
		, EffectClass(MoveTemp(InEffectClass))
		, PortFamilyId(MoveTemp(InPortFamilyId))
	{
	}

	const FString& FUeAgentTimelineAnimationCapabilityContract::GetCapabilityId() const { return CapabilityId; }
	const FString& FUeAgentTimelineAnimationCapabilityContract::GetDescriptorId() const { return DescriptorId; }
	int32 FUeAgentTimelineAnimationCapabilityContract::GetVersion() const { return Version; }
	const FString& FUeAgentTimelineAnimationCapabilityContract::GetOperationKind() const { return OperationKind; }
	const FString& FUeAgentTimelineAnimationCapabilityContract::GetEffectClass() const { return EffectClass; }
	const FString& FUeAgentTimelineAnimationCapabilityContract::GetPortFamilyId() const { return PortFamilyId; }
	const FString& FUeAgentTimelineAnimationCapabilityContract::GetTemporalContractId() const { return TemporalContractId; }
	const FString& FUeAgentTimelineAnimationCapabilityContract::GetSpaceContractId() const { return SpaceContractId; }

	const TArray<FUeAgentTimelineAnimationPortFamilyContract>&
	GetTimelineAnimationPortFamilyContracts()
	{
		static const TArray<FUeAgentTimelineAnimationPortFamilyContract>
			Contracts = {
				{
					TEXT("temporal_read"),
					{TEXT("asset_or_subject_ref"), TEXT("state_plane"),
					 TEXT("source_revision")},
					{TEXT("authored")},
					TEXT("explicit_subject_clock_and_rate"),
					TEXT("typed_open_inclusive_exclusive"),
					TEXT("optional_pose_observation_uses_uai_space_contract_v1"),
					TEXT("independent_asset_or_sample_readback"),
				},
				{
					TEXT("animation_keyed_data"),
					{TEXT("asset_ref"), TEXT("source_revision"),
					 TEXT("temporal_domain"), TEXT("stable_item_refs")},
					{TEXT("authored")},
					TEXT("convert_through_c4_to_asset_sample_clock"),
					TEXT("typed_event_and_track_ranges"),
					TEXT("bone_values_require_explicit_skeletal_frame"),
					TEXT("independent_tracks_curves_events_readback"),
				},
				{
					TEXT("timeline_structure"),
					{TEXT("owner_or_target_ref"), TEXT("source_revision"),
					 TEXT("temporal_domain"), TEXT("stable_item_refs")},
					{TEXT("authored")},
					TEXT("explicit_montage_or_sequence_clock"),
					TEXT("typed_section_segment_key_ranges"),
					TEXT("transform_channels_require_uai_space_contract_v1"),
					TEXT("independent_structure_identity_and_range_readback"),
				},
				{
					TEXT("subject_animation_state"),
					{TEXT("world"), TEXT("target_refs"),
					 TEXT("temporal_domain")},
					{TEXT("authored")},
					TEXT("explicit_subject_evaluation_clock"),
					TEXT("point_or_explicit_evaluation_range"),
					TEXT("pose_uses_explicit_skeletal_frame_morph_is_scalar"),
					TEXT("same_component_pose_or_morph_readback"),
				},
				{
					TEXT("timeline_rig_candidate"),
					{TEXT("sequence"), TEXT("rig"), TEXT("goal"),
					 TEXT("probe_session"), TEXT("source_revision"),
					 TEXT("temporal_domain"), TEXT("space_contract")},
					{TEXT("authored")},
					TEXT("explicit_sequence_clock_and_rate"),
					TEXT("typed_key_or_section_range"),
					TEXT("candidate_and_readback_share_explicit_c4_conversion_chain"),
					TEXT("independent_sequence_and_rig_key_readback"),
				},
			};
		return Contracts;
	}

	const TArray<FUeAgentTimelineAnimationCapabilityContract>&
	GetTimelineAnimationCapabilityContracts()
	{
		static const TArray<FUeAgentTimelineAnimationCapabilityContract>
			Contracts = {
				{TEXT("animation.sequence.read"), TEXT("uai.query.animation.sequence.read"), 1, TEXT("query"), TEXT("read_only"), TEXT("temporal_read")},
				{TEXT("animation.sequence.root_motion.analyze"), TEXT("uai.query.animation.sequence.root_motion.analyze"), 1, TEXT("query"), TEXT("read_only"), TEXT("temporal_read")},
				{TEXT("animation.sequence.pose.sample"), TEXT("uai.query.animation.sequence.pose.sample"), 1, TEXT("query"), TEXT("read_only"), TEXT("temporal_read")},
				{TEXT("animation.blend_space.sample"), TEXT("uai.query.animation.blend_space.sample"), 1, TEXT("query"), TEXT("read_only"), TEXT("temporal_read")},
				{TEXT("animation.sequence.bone_tracks.replace"), TEXT("uai.operation.animation.sequence.bone_tracks.replace"), 1, TEXT("operation"), TEXT("write"), TEXT("animation_keyed_data")},
				{TEXT("animation.sequence.curves.update"), TEXT("uai.operation.animation.sequence.curves.update"), 1, TEXT("operation"), TEXT("write"), TEXT("animation_keyed_data")},
				{TEXT("animation.sequence.metadata.update"), TEXT("uai.operation.animation.sequence.metadata.update"), 1, TEXT("operation"), TEXT("write"), TEXT("animation_keyed_data")},
				{TEXT("animation.sequence.events.update"), TEXT("uai.operation.animation.sequence.events.update"), 1, TEXT("operation"), TEXT("write"), TEXT("animation_keyed_data")},
				{TEXT("animation.montage.read"), TEXT("uai.query.animation.montage.read"), 1, TEXT("query"), TEXT("read_only"), TEXT("temporal_read")},
				{TEXT("animation.montage.create"), TEXT("uai.operation.animation.montage.create"), 1, TEXT("operation"), TEXT("write"), TEXT("timeline_structure")},
				{TEXT("animation.montage.structure.replace"), TEXT("uai.operation.animation.montage.structure.replace"), 1, TEXT("operation"), TEXT("write"), TEXT("timeline_structure")},
				{TEXT("animation.skeleton.slots.read"), TEXT("uai.query.animation.skeleton.slots.read"), 1, TEXT("query"), TEXT("read_only"), TEXT("temporal_read")},
				{TEXT("timeline.sequence.read"), TEXT("uai.query.timeline.sequence.read"), 1, TEXT("query"), TEXT("read_only"), TEXT("temporal_read")},
				{TEXT("timeline.sequence.create"), TEXT("uai.operation.timeline.sequence.create"), 1, TEXT("operation"), TEXT("write"), TEXT("timeline_structure")},
				{TEXT("timeline.sequence.structure.replace"), TEXT("uai.operation.timeline.sequence.structure.replace"), 1, TEXT("operation"), TEXT("write"), TEXT("timeline_structure")},
				{TEXT("timeline.widget_animation.read"), TEXT("uai.query.timeline.widget_animation.read"), 1, TEXT("query"), TEXT("read_only"), TEXT("temporal_read")},
				{TEXT("animation.subject.pose.update"), TEXT("uai.operation.animation.subject.pose.update"), 1, TEXT("operation"), TEXT("session_control"), TEXT("subject_animation_state")},
				{TEXT("animation.subject.morph_weights.update"), TEXT("uai.operation.animation.subject.morph_weights.update"), 1, TEXT("operation"), TEXT("session_control"), TEXT("subject_animation_state")},
				{TEXT("animation.ik_probe_candidate.apply"), TEXT("uai.operation.animation.ik_probe_candidate.apply"), 1, TEXT("operation"), TEXT("write"), TEXT("timeline_rig_candidate")},
			};
		return Contracts;
	}
}

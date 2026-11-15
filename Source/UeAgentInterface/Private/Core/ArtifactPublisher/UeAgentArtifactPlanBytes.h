#pragma once

namespace UE::UAI::HjsonWorkspace { class FExportFilePlan; }

namespace UE::UAI::ArtifactPublisher
{
    // Module-internal producer handoff to the publisher's existing immutable
    // sealed file. Caller has computed this plan's digest from the exact bytes;
    // the plan owns those sealed bytes. No source/path or publication authority.
    void RememberVerifiedPlanBytes(const HjsonWorkspace::FExportFilePlan& File);
}

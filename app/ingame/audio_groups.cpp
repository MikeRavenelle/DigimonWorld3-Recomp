#include "audio_groups.h"

#include "audio_mix.h"
#include "dw3/recomp.h"

namespace dw3app::ingame {
namespace {

// libsnd's voice table: 56 bytes per voice, the sequence id of the note that was last keyed on it at +0x20.
constexpr uint32_t kVoiceTable = 0x800815C0, kVoiceStride = 56, kVoiceSequence = 0x20;

}  // namespace

AudioGroups::AudioGroups(dw3::Machine& machine, SettingsController& controller) : machine_(machine), controller_(controller) {
    machine_.spu().set_voice_classifier(this);
    apply();
}

AudioGroups::~AudioGroups() { machine_.spu().set_voice_classifier(nullptr); }

void AudioGroups::apply() {
    const Settings::Audio& audio = controller_.active().audio;
    machine_.spu().set_group_volume(dw3::VoiceGroup::Music, audio.music_volume);
    machine_.spu().set_group_volume(dw3::VoiceGroup::Effects, audio.sfx_volume);
}

dw3::VoiceGroup AudioGroups::classify(int voice) {
    const uint16_t sequence = dw3_read16(&machine_.ctx(), kVoiceTable + kVoiceStride * static_cast<uint32_t>(voice) + kVoiceSequence);
    return is_music_sequence(sequence) ? dw3::VoiceGroup::Music : dw3::VoiceGroup::Effects;
}

}  // namespace dw3app::ingame

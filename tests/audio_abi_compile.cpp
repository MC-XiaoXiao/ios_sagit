// SPDX-License-Identifier: BSD-2-Clause
#include <IOKit/audio/IOAudio2Device.h>
// Keep every base hook in the object so the import gate checks its exact
// compiler-generated signature as well as the 197-slot native layout.
class AudioABIProbe : public IOAudio2Device {
    OSDeclareDefaultStructors(AudioABIProbe);
};
OSDefineMetaClassAndStructors(AudioABIProbe, IOAudio2Device);

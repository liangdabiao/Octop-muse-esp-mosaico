Muse Gadget SDK, https://github.com/facebookincubator/muse-gadget-sdk
Pinned commit: 693cde9a884ad1edc87251b9f8944815f8de4809
Apache-2.0. upstream/ files retain upstream copyright.
Local changes: remove SDK token prefix logs; read the maker token from the immutable boot-loaded NVS configuration;
increase the bounded text accumulator to 4096 bytes and expose a final-reply getter;
complete a text turn after its final quiet period independently of upstream caption scrolling.
Adaptations: headless GSP hooks; disabled OTA implementation and L3 tunnel stubs;
no app_main from SDK; retained Recovery owns firmware writes.
The SDK's cloud-dependent OTA rollback monitor is compiled only when its OTA
feature is enabled; ESP-Iris owns application acceptance in this project.
Muse-enabled control sessions reserve protocol-maximum inbound scratch on PSRAM
boards and 17 KiB on small boards, covering 16 KiB chat body chunks and envelopes;
inbound HTTP response header storage is expanded to 64 entries.
No upstream avatar included. Other optional board drivers are excluded from compilation.
The voice-worker-owned muse_companion_language() adapter adds a Chinese/English
reply instruction to the existing /chat/stream message alongside the voice attachment.
This is a conversational instruction, not a server-enforced translation setting.
Typed generation turns share /chat/stream and /chat/subscribe with the paired
device's Noise transport. A bounded 96 KiB subscription-line gather and a
receive-lock observer let the app keep up to 64 KiB of generated source in
PSRAM without changing the 4 KiB voice-caption limit. Typed turns allow up to
five minutes for generation; normal voice timeouts remain unchanged.

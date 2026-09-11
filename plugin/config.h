#define PLUG_NAME "Seedlathe"
#define PLUG_MFR "Seedlathe"
#define PLUG_VERSION_HEX 0x00000100
#define PLUG_VERSION_STR "0.1.0"
#define PLUG_UNIQUE_ID 'Sdla'
#define PLUG_MFR_ID 'Sdlt'
#define PLUG_URL_STR "https://seedlathe.com"
#define PLUG_EMAIL_STR "hello@seedlathe.com"
#define PLUG_COPYRIGHT_STR "Copyright 2026 Seedlathe"
#define PLUG_CLASS_NAME Seedlathe

#define BUNDLE_NAME "Seedlathe"
#define BUNDLE_MFR "Seedlathe"
#define BUNDLE_DOMAIN "com"

#define PLUG_CHANNEL_IO "0-2"
#define SHARED_RESOURCES_SUBPATH "Seedlathe"

#define PLUG_LATENCY 0
#define PLUG_TYPE 1
#define PLUG_DOES_MIDI_IN 1
#define PLUG_DOES_MIDI_OUT 0
#define PLUG_DOES_MPE 0
#define PLUG_DOES_STATE_CHUNKS 1
#define PLUG_HAS_UI 1
#define PLUG_WIDTH 1280
#define PLUG_HEIGHT 860
#define PLUG_FPS 60
#define PLUG_SHARED_RESOURCES 0
#define PLUG_HOST_RESIZE 0

#define AUV2_ENTRY Seedlathe_Entry
#define AUV2_ENTRY_STR "Seedlathe_Entry"
#define AUV2_FACTORY Seedlathe_Factory
#define AUV2_VIEW_CLASS Seedlathe_View
#define AUV2_VIEW_CLASS_STR "Seedlathe_View"

#define AAX_TYPE_IDS 'SDL1'
#define AAX_PLUG_MFR_STR "Seedlathe"
#define AAX_PLUG_NAME_STR "Seedlathe\nSDLA"
#define AAX_DOES_AUDIOSUITE 0
#define AAX_PLUG_CATEGORY_STR "Synth"

#define VST3_SUBCATEGORY "Instrument|Synth"
#define CLAP_MANUAL_URL "https://seedlathe.com/manual"
#define CLAP_SUPPORT_URL "https://seedlathe.com/support"
#define CLAP_DESCRIPTION "Seed-driven procedural synthesizer"
#define CLAP_FEATURES "instrument", "synthesizer"

#define APP_NUM_CHANNELS 2
#define APP_N_VECTOR_WAIT 0
#define APP_MULT 1
#define APP_COPY_AUV3 0
#define APP_SIGNAL_VECTOR_SIZE 64

// Referenced by resources/main.rc, which embeds the font as a Windows resource.
#define ROBOTO_FN "Roboto-Regular.ttf"

// Catalog of the text the host shows the player (ui_text.h). One array per catalog language, indexed by UiTextId: a new
// string gets an id in ui_text.h and an entry in every language that has one (a missing entry falls back to English).
// The menu has English and the menu languages (menu_lang); the software keyboard (the game's name entry) has a row for
// every game language.
#include <stdatomic.h>
#include <string.h>

#include "cfg.h"
#include "ov.h"
#include "platform/platform.h"
#include "ui_text.h"

typedef struct { const char *lang; const char *const *text; } Row;

// The language names are the same in every row.
#define LANG_NAMES \
    [UI_MENU_LANGNAME_EN] = "English", [UI_MENU_LANGNAME_JA] = "日本語", [UI_MENU_LANGNAME_FR] = "Français", \
    [UI_MENU_LANGNAME_DE] = "Deutsch", [UI_MENU_LANGNAME_IT] = "Italiano", [UI_MENU_LANGNAME_ES] = "Español", \
    [UI_MENU_LANGNAME_NL] = "Nederlands", [UI_MENU_LANGNAME_RU] = "Русский", \
    [UI_MENU_LANGNAME_ZH_HANS] = "简体中文", [UI_MENU_LANGNAME_ZH_HANT] = "繁體中文", [UI_MENU_LANGNAME_KO] = "한국어", \
    [UI_MENU_LANGNAME_PT_BR] = "Portuguese (Brazil)"

// SWKBD: TITLE, PROMPT, DEFAULT_NAME (at most 9 UTF-16 units, no '@' '%' '\'), OK, CANCEL, PAD_HINT.
#define SWKBD(title, prompt, name, ok, cancel, hint) \
    [UI_SWKBD_TITLE] = title, [UI_SWKBD_PROMPT] = prompt, [UI_SWKBD_DEFAULT_NAME] = name, [UI_SWKBD_OK] = ok, \
    [UI_SWKBD_CANCEL] = cancel, [UI_SWKBD_PAD_HINT] = hint, LANG_NAMES

static const char *const text_en[UI_TEXT_COUNT] = {
    [UI_MENU_QUICK_TITLE] = "Age of Calamity Recomp  ·  quick menu",
    [UI_MENU_QUICK_HINT] = "Hold L3+R3 to open or close. B closes.",
    [UI_MENU_QUICK_FPS] = "Frame rate",
    [UI_MENU_QUICK_FPS_DESC] = "30 FPS is the game's own rate; 60 FPS draws twice as many frames at the same game speed and needs about twice the processor time; 120, 144 and 240 FPS are experimental (in development, not optimized yet) and need a monitor that fast. For more battery, stay at 30 and also lower the TDP in the Steam performance menu.",
    [UI_MENU_QUICK_RES] = "Console mode",
    [UI_MENU_QUICK_RES_DESC] = "Whether the game thinks the Switch is docked (TV) or in handheld mode, as when you dock it. Age of Calamity draws at the same size in both: the picture size is the Game resolution of the full menu.",
    [UI_MENU_QUICK_RES_HANDHELD] = "Handheld",
    [UI_MENU_QUICK_RES_DOCKED] = "TV (docked)",
    [UI_MENU_QUICK_FULL] = "Full menu",
    [UI_MENU_QUICK_MARK] = "Mark this moment",
    SWKBD("Text entry", "Enter a name", "Horse", "OK", "Cancel", "Controller: A = OK, B = Cancel"),

    [UI_MENU_TITLE] = "Age of Calamity Recomp",
    [UI_MENU_TAB_SCREEN] = "Screen", [UI_MENU_TAB_GAME] = "Game", [UI_MENU_TAB_MODS] = "Mods", [UI_MENU_TAB_SOUND] = "Sound",
    [UI_MENU_TAB_CONTROLS] = "Controls", [UI_MENU_TAB_AMIIBO] = "Amiibo", [UI_MENU_TAB_ADVANCED] = "Advanced",
    [UI_MENU_TAB_LANGUAGE] = "Language",
    [UI_MENU_CONTINUE] = "Continue", [UI_MENU_QUIT] = "Quit game",
    [UI_MENU_ON] = "On", [UI_MENU_OFF] = "Off", [UI_MENU_AUTO] = "Automatic", [UI_MENU_DEFAULT_MARK] = "default",
    [UI_MENU_AFTER_RESTART] = "after restart", [UI_MENU_SET_BY_ENV] = "set by environment variable",
    [UI_MENU_SAVE_FAILED] = "could not save settings",
    [UI_MENU_VERSION] = "Settings  ·  version %s  ·  game %s",

    [UI_MENU_FULLSCREEN] = "Fullscreen", [UI_MENU_FULLSCREEN_DESC] = "Fills the screen with the game; F11 toggles it.",
    [UI_MENU_WINDOW_SIZE] = "Window size", [UI_MENU_WINDOW_SIZE_DESC] = "Size of the game window when not fullscreen.",
    [UI_MENU_PRESENT] = "Presentation", [UI_MENU_PRESENT_DESC] = "How finished frames reach the screen.",
    [UI_MENU_PRESENT_MAILBOX] = "Mailbox", [UI_MENU_PRESENT_VSYNC] = "VSync", [UI_MENU_PRESENT_IMMEDIATE] = "Immediate",
    [UI_MENU_PRESENT_ADAPTIVE] = "Adaptive VSync",
    [UI_MENU_PRESENT_IN_USE] = "in use: %s",
    [UI_MENU_GPU] = "Graphics card", [UI_MENU_GPU_DESC] = "The Vulkan device that draws the game.",
    [UI_MENU_GPU_AUTO] = "Automatic",
    [UI_MENU_PERF_OVERLAY] = "Performance overlay",
    [UI_MENU_PERF_OVERLAY_DESC] = "Frame rate in the top right corner. Full adds frame times with a graph, CPU, GPU and "
                                  "memory load, temperatures and the render thread's waits. F10 cycles the modes.",
    [UI_MENU_PERF_OFF] = "Off", [UI_MENU_PERF_FPS] = "FPS", [UI_MENU_PERF_FULL] = "Full",
    // printf format of the overlay line in FPS mode: the frame rate
    [UI_PERF_LINE_FPS] = "%.0f fps",
    [UI_MENU_GRAPHICS] = "Picture",
    [UI_MENU_OUTPUT_RES] = "Game resolution",
    [UI_MENU_OUTPUT_RES_DESC] = "The size the game draws its picture and interface in; 1920 x 1080 is the game's own. 1280 x 720 matches the "
                                "Steam Deck's screen and spares the GPU; below 1920 x 1080 the 3D scene can go up to 85 %. "
                                "2560 x 1440 and 3840 x 2160 draw the interface sharp on large screens, for more GPU work.",
    [UI_MENU_RENDER_SCALE] = "3D render scale",
    [UI_MENU_RENDER_SCALE_DESC] = "Size of the 3D scene relative to the game resolution; the game's FSR scales it to "
                                  "that size. 100 % draws it at full size; 125 to 200 % draw it larger and scale it down "
                                  "(supersampling): the sharpest, for about twice the GPU work at 140 % and four times "
                                  "at 200 %. Below the 1920 x 1080 game resolution it stops at 85 %, and the scene keeps "
                                  "at most 3840 x 2160 pixels (200 % of 1920 x 1080, 100 % of 3840 x 2160).",
    [UI_MENU_RENDER_SCALE_GAME] = "Game (75 %)",
    [UI_MENU_DYNAMIC_RES] = "Dynamic resolution",
    [UI_MENU_DYNAMIC_RES_DESC] = "In heavy fights the game draws the 3D scene smaller for a moment to keep its frame rate "
                                 "(down to 70 % of its height in a large charge across Hyrule Field). "
                                 "Off keeps the scene at the size of the 3D render scale at all times: sharper in those "
                                 "moments, for more GPU work. Applies at once.",
    [UI_MENU_DYNAMIC_RES_GAME] = "Game", [UI_MENU_DYNAMIC_RES_OFF] = "Off (full size)",
    [UI_MENU_UPSCALER] = "Scaling to the window",
    [UI_MENU_UPSCALER_DESC] = "How the picture is scaled when the window is larger than the game resolution. FSR keeps "
                              "edges and detail sharper than bilinear.",
    [UI_MENU_UPSCALER_FSR] = "AMD FSR 1", [UI_MENU_UPSCALER_BILINEAR] = "Bilinear",
    [UI_MENU_ASPECT] = "Picture shape",
    [UI_MENU_ASPECT_DESC] = "When the window's shape differs from the game resolution's (a 16:9 picture on a 21:9 "
                            "screen): keep the picture's shape with black bars, or stretch it over the window.",
    [UI_MENU_ASPECT_FIT] = "Black bars", [UI_MENU_ASPECT_STRETCH] = "Stretch",
    [UI_MENU_ANISO] = "Texture filtering",
    [UI_MENU_ANISO_DESC] = "Anisotropic filtering of surface textures: ground, roads and walls seen at a shallow angle stay sharp into the distance instead of blurring. Game: the level the game asks for, as on the Switch.",
    [UI_MENU_ANISO_GAME] = "Game",
    [UI_MENU_SHARPNESS] = "FSR sharpness",
    [UI_MENU_SHARPNESS_DESC] = "Sharpening after FSR scaling; 0 turns it off.",
    [UI_MENU_COLOR] = "Colour",
    [UI_MENU_COLOR_PRESET] = "Colour profile",
    [UI_MENU_COLOR_PRESET_DESC] = "Sets the four values below. Vivid: more contrast and saturation, shadows opened a "
                                  "little (made for OLED screens). Clarity: a little more contrast, saturation and "
                                  "brightness. Moving a value makes the profile Custom. The HUD changes with the picture; "
                                  "this menu does not.",
    [UI_MENU_COLOR_DEFAULT] = "Game's own", [UI_MENU_COLOR_VIVID] = "Vivid (OLED)", [UI_MENU_COLOR_CLARITY] = "Clarity",
    [UI_MENU_COLOR_CUSTOM] = "Custom",
    [UI_MENU_COLOR_BRIGHTNESS] = "Brightness", [UI_MENU_COLOR_BRIGHTNESS_DESC] = "Added to every colour; 0 leaves it.",
    [UI_MENU_COLOR_CONTRAST] = "Contrast",
    [UI_MENU_COLOR_CONTRAST_DESC] = "Spreads colours away from mid-grey (above 100 %) or towards it; 100 % leaves them.",
    [UI_MENU_COLOR_SATURATION] = "Saturation",
    [UI_MENU_COLOR_SATURATION_DESC] = "Colour strength against grey; 100 % leaves it.",
    [UI_MENU_COLOR_GAMMA] = "Gamma",
    [UI_MENU_COLOR_GAMMA_DESC] = "Below 1 brightens dark and middle tones, above 1 darkens them; 1 leaves them.",
    [UI_MENU_HDR_GROUP] = "HDR",
    [UI_MENU_HDR] = "HDR output",
    [UI_MENU_HDR_DESC] = "Bright highlights on an HDR display. Needs HDR turned on in Windows (\"Use HDR\") or in a "
                         "Wayland compositor with colour management; without it the picture stays SDR. Applies after a "
                         "restart. The colour profile above is for SDR and is not used in HDR.",
    [UI_MENU_HDR_PEAK] = "Peak brightness",
    [UI_MENU_HDR_PEAK_DESC] = "Your display's peak in nits; the brightest highlights stop there.",
    [UI_MENU_HDR_GAME] = "Game brightness",
    [UI_MENU_HDR_GAME_DESC] = "Nits of the picture's white (paper white). 203 is the HDR reference; raise it in a "
                              "bright room.",
    [UI_MENU_HDR_UI] = "HUD and menu brightness",
    [UI_MENU_HDR_UI_DESC] = "Nits of the HUD's and this menu's white.",
    [UI_MENU_SHADOW_SCALE] = "Shadow resolution",
    [UI_MENU_SHADOW_SCALE_DESC] = "Draws the sun's shadow map this many times larger on each side than the game's 1024x2048: "
                                  "sharper shadow edges and leaf shadows. Takes effect at the next start.",
    [UI_MENU_PCSS] = "Soft shadows",
    [UI_MENU_PCSS_DESC] = "Sun shadows sharp where an object meets the ground and softer the further the shadow falls "
                          "(PCSS). 0 keeps the game's filter. Takes effect at the next start.",
    [UI_MENU_DLAA] = "DLSS / FSR 3.1",
    [UI_MENU_DLAA_DESC] = "Temporal anti-aliasing and upscaling of the 3D scene: smoother edges and less shimmer. NVIDIA "
                          "DLSS on RTX cards (nvngx_dlss.dll next to AgeOfCalamityRecomp), AMD FSR 3.1 on any other "
                          "graphics card (the engine below). The HUD is not touched. The mode below sets the resolution "
                          "it works from. Takes effect after a restart.",
    [UI_MENU_DLAA_ENGINE] = "Upscaler",
    [UI_MENU_DLAA_ENGINE_DESC] = "Which one runs. Automatic: NVIDIA DLSS where the card and nvngx_dlss.dll have it, "
                                 "else AMD FSR 3.1. FSR 3.1 works on AMD, Intel and NVIDIA cards. Takes effect after a "
                                 "restart.",
    [UI_MENU_DLAA_ENGINE_AUTO] = "Automatic", [UI_MENU_DLAA_ENGINE_DLSS] = "NVIDIA DLSS",
    [UI_MENU_DLAA_ENGINE_FSR3] = "AMD FSR 3.1",
    [UI_MENU_FSR3_SHARPNESS] = "FSR 3.1 sharpness",
    [UI_MENU_FSR3_SHARPNESS_DESC] = "FSR 3.1's own sharpening of the 3D scene, against the softness of its temporal "
                                    "accumulation (FSR 3.1 only); 0 turns it off. Applies at once.",
    [UI_MENU_DLSS] = "Mode",
    [UI_MENU_DLSS_DESC] = "Native (DLAA) draws the 3D scene at full resolution. Quality, Balanced and Performance draw it at "
                          "67, 58 and 50 % and let DLSS or FSR 3.1 scale it up, for more frames per second; they replace the 3D render "
                          "scale. Takes effect after a restart.",
    [UI_MENU_DLSS_DLAA] = "Native (DLAA)", [UI_MENU_DLSS_QUALITY] = "Quality", [UI_MENU_DLSS_BALANCED] = "Balanced",
    [UI_MENU_DLSS_PERFORMANCE] = "Performance",
    [UI_MENU_DLSS_PRESET] = "DLSS model",
    [UI_MENU_DLSS_PRESET_DESC] = "Which DLSS model runs (DLSS only). Automatic follows NVIDIA's advice per mode: K for DLAA, Quality "
                                 "and Balanced, M for Performance. J: slightly less ghosting behind moving characters, "
                                 "a little more flicker. K: the sharpest model in every mode. Applies at once.",
    [UI_MENU_DLSS_PRESET_DEFAULT] = "Automatic", [UI_MENU_DLSS_PRESET_K] = "K (sharpest)",
    [UI_MENU_DLSS_PRESET_J] = "J (less ghosting)",
    [UI_MENU_FXAA] = "Anti-aliasing (FXAA)",
    [UI_MENU_FXAA_DESC] = "Smooths the jagged edges of the 3D scene; the HUD stays sharp. It softens fine detail a little. Off while DLAA or DLSS is on.",

    [UI_MENU_FPS_LIMIT] = "Frame rate",
    [UI_MENU_FPS_LIMIT_DESC] = "30 FPS is the game's own rate. At 60 FPS the game draws twice as many frames and still "
                               "runs at the right speed; it needs about twice the processor time, so a busy battle can "
                               "dip below 60. 120, 144 and 240 FPS need a monitor that fast and a lot more processor "
                               "time, and are experimental: still in development, not optimized yet (busy battles stay "
                               "under 120 on a fast processor). Applies at once.",
    [UI_MENU_FPS_30] = "30 FPS", [UI_MENU_FPS_60] = "60 FPS", [UI_MENU_FPS_120] = "120 FPS (experimental)",
    [UI_MENU_FPS_144] = "144 FPS (experimental)", [UI_MENU_FPS_240] = "240 FPS (experimental)",
    [UI_MENU_GAME_LANG] = "Game language",
    [UI_MENU_GAME_LANG_DESC] = "Language of the game's text and voices. Automatic follows the system's language.",
    [UI_MENU_GAME_LANG_AUTO] = "Automatic",
    [UI_MENU_OPERATION_MODE] = "Console mode",
    [UI_MENU_OPERATION_MODE_DESC] = "Whether the game thinks it runs docked or handheld.",
    [UI_MENU_MODE_DOCKED] = "Docked", [UI_MENU_MODE_HANDHELD] = "Handheld",
    [UI_MENU_FRAME_QUEUE] = "Frame queue",
    [UI_MENU_FRAME_QUEUE_DESC] = "The game starts a frame while the previous one waits for its turn on screen: far fewer "
                                 "dropped frames at the limit, up to one frame more input delay. Applies at once.",
    [UI_MENU_ASSUME_FOCUS] = "Keep running out of focus",
    [UI_MENU_ASSUME_FOCUS_DESC] = "On (the default), the game keeps running while you are in another window, and the "
                                  "keyboard stays quiet until the window is in front again. Off, it pauses when its "
                                  "window loses focus, as on the Switch when Home is pressed; a load interrupted that "
                                  "way can stop for good.",

    [UI_MENU_MODS_INSTALLED] = "Installed mods",
    [UI_MENU_MODS_NONE] = "No mods yet. Unpack a mod into the mods folder, one folder per mod with its romfs folder "
                          "inside, and restart the game. TKMM can export its merged mods into a folder there too.",
    [UI_MENU_MODS_OPEN] = "Open mods folder",
    [UI_MENU_MODS_OPEN_DESC] = "Shows the folder the game loads mods from. It is created if it is missing.",
    [UI_MENU_MODS_ROW_DESC] = "Game files it replaces: %u. New files it adds: %u.",
    [UI_MENU_MODS_OVERRIDDEN] = "%u of its files come from %s, which is higher in the list.",
    [UI_MENU_MODS_MOVE_HINT] = "The top face button or Ctrl+Up moves it up, the left face button or Ctrl+Down moves it "
                               "down. When two mods change the same file, the higher one wins. Changes apply after a "
                               "restart.",
    [UI_MENU_MODS_EMPTY] = "This folder has no romfs, exefs or cheats files.",
    [UI_MENU_MODS_SUBSDK] = "This mod brings its own game code (a subsdk plugin or a replaced main). The port cannot "
                            "load code modules, so the whole mod stays off.",
    [UI_MENU_MODS_EXEFS_LATER] = "This mod patches the game code (exefs). The game builds the patches in when it "
                                 "starts next (a rebuild of a few minutes); until then the whole mod stays off.",
    [UI_MENU_MODS_CHEATS] = "Cheats: %d. Switch them on the Cheats tab.",
    [UI_MENU_MODS_CHEATS_FILE] = "Cheats for this game version are read at start from cheats\\%s in this mod.",
    [UI_MENU_MODS_BAD_NAME] = "The folder name contains |, which the mod list uses between names. Rename the folder.",
    [UI_MENU_MODS_OTHER_VERSION] = "%u of its files are made for another game version and are left out. Look for a "
                                   "version of this mod made for your game version.",
    [UI_MENU_MODS_UNUSED] = "not used",

    [UI_MENU_VOLUME] = "Volume", [UI_MENU_VOLUME_DESC] = "Master volume of the game.",
    [UI_MENU_MUTE] = "Mute", [UI_MENU_MUTE_DESC] = "Silences the game without changing the volume.",
    [UI_MENU_MUTE_BG] = "Mute in the background",
    [UI_MENU_MUTE_BG_DESC] = "Silences the game while another window is active.",
    [UI_MENU_AUDIO_DEVICE] = "Output device",
    [UI_MENU_AUDIO_DEVICE_DESC] = "Where the sound plays. System default follows the Windows default device. A chosen "
                                  "device that is unplugged falls back to the system default until it is back. "
                                  "Applies at once.",
    [UI_MENU_AUDIO_DEVICE_DEFAULT] = "System default",
    [UI_MENU_INVERT_LX] = "Invert left stick X", [UI_MENU_INVERT_LY] = "Invert left stick Y",
    [UI_MENU_INVERT_RX] = "Invert right stick X (camera)", [UI_MENU_INVERT_RY] = "Invert right stick Y (camera)",
    [UI_MENU_INVERT_DESC] = "Mirrors this axis of the gamepad's stick.",

    [UI_MENU_MOTION] = "Motion controls", [UI_MENU_MOTION_DESC] = "Aiming with the controller's gyroscope.",
    [UI_MENU_GYRO_CALIBRATION] = "Gyro calibration",
    [UI_MENU_GYRO_CALIBRATION_DESC] = "Takes the measured drift off the gyroscope. Off: the raw gyroscope.",
    [UI_MENU_CALIBRATE_GYRO] = "Calibrate gyro",
    [UI_MENU_CALIBRATE_GYRO_DESC] = "Put the controller down on a flat surface, press, and leave it still for 2 "
                                    "seconds. It stops the aim drifting to the side. Kept for this controller.",
    [UI_MENU_CALIB_NONE] = "not calibrated", [UI_MENU_CALIB_COLLECTING] = "keep it still...",
    [UI_MENU_CALIB_DONE] = "calibrated", [UI_MENU_CALIB_FAILED] = "no gyro data",
    [UI_MENU_CALIB_MOVED] = "it moved, try again",
    [UI_MENU_RUMBLE_STRENGTH] = "Vibration strength",
    [UI_MENU_RUMBLE_STRENGTH_DESC] = "How strong the controller vibrates.",
    [UI_MENU_RUMBLE_CURVE] = "Vibration curve",
    [UI_MENU_RUMBLE_CURVE_DESC] = "Lower values make weak vibration stronger.",
    [UI_MENU_RUMBLE_TRIGGERS] = "Trigger vibration (Xbox)",
    [UI_MENU_RUMBLE_TRIGGERS_DESC] = "Vibrates the triggers of an Xbox controller too.",
    [UI_MENU_PAD_LAYOUT] = "Button layout",
    [UI_MENU_PAD_LAYOUT_DESC] = "Which button is A. Nintendo: the right one, as on a Switch (circle A, cross B, square "
                                "Y, triangle X on a PlayStation controller). Xbox: the bottom one, as labelled on an "
                                "Xbox controller. PlayStation: cross, the bottom one, is A, circle B, square X and "
                                "triangle Y. Automatic picks Nintendo for Nintendo and PlayStation controllers and Xbox "
                                "for the others. Button prompts below picks the icons the game shows.",
    [UI_MENU_PAD_LAYOUT_AUTO] = "Automatic", [UI_MENU_PAD_LAYOUT_NINTENDO] = "Nintendo (A right)",
    [UI_MENU_PAD_LAYOUT_XBOX] = "Xbox (A bottom)",
    [UI_MENU_PAD_LAYOUT_PLAYSTATION] = "PlayStation (cross A)",
    [UI_MENU_PAD_LAYOUT_IN_USE] = "in use: %s",
    [UI_MENU_PROMPTS] = "Button prompts",
    [UI_MENU_PROMPTS_DESC] = "Which controller's buttons the game's prompts show. Nintendo: the game's own. Xbox: LB, "
                             "RB, LT, RT, Menu and View, and A, B, X and Y as labelled on the button "
                             "pressed (A is on the right button as on the Switch, labelled B on an Xbox pad, so the "
                             "prompts for A show B). PlayStation: L1, R1, L2, R2, Options and Create, and cross, "
                             "circle, square and triangle the same way (A shows circle). Applies at once. They are "
                             "part of the game's files, so a button you remap below keeps its old prompt on screen.",
    [UI_MENU_PROMPTS_NINTENDO] = "Nintendo", [UI_MENU_PROMPTS_XBOX] = "Xbox",
    [UI_MENU_PROMPTS_PLAYSTATION] = "PlayStation",
    [UI_MENU_PROMPTS_MISSING] = "prompt files missing: run the setup again",
    [UI_MENU_PROMPTS_FAILED] = "prompt files damaged: run the setup again",
    [UI_MENU_PROMPTS_RESTART] = "prompts change after restart",
    [UI_MENU_RESTART] = "Restart now",
    [UI_MENU_RESTART_DESC] = "Closes the game and starts it again with the new prompts. Progress since your last save "
                             "is lost.",
    [UI_MENU_RESTART_CONFIRM] = "Restart the game now? Progress since your last save is lost.",
    [UI_MENU_RESTART_YES] = "Restart",
    [UI_MENU_RESTART_SAVING] = "The game is saving: Restart is possible in a moment.",
    [UI_MENU_LSTICK_DEADZONE] = "Left stick dead zone", [UI_MENU_RSTICK_DEADZONE] = "Right stick dead zone",
    [UI_MENU_STICK_DEADZONE_DESC] = "How far a gamepad stick moves before the game sees it. Raise it if the character or the camera moves on its own: let go of the stick, read \"stick now\" and set the dead zone a few percent above it.",
    [UI_MENU_STICK_DEADZONE_NOW] = "stick now: %.0f %%",
    [UI_MENU_MOUSE_SENS] = "Mouse camera sensitivity",
    [UI_MENU_MOUSE_SENS_DESC] = "How fast the mouse turns the camera.",
    [UI_MENU_WALK_KEY] = "Walk key",
    [UI_MENU_WALK_KEY_DESC] = "Hold it to tilt the keyboard's sticks part of the way, so the keyboard walks instead of "
                              "runs. The walk scale sets how far. A controller and the mouse are not affected.",
    [UI_MENU_WALK_KEY_TAKEN] = "also %s",
    [UI_MENU_WALK_SCALE] = "Walk scale",
    [UI_MENU_WALK_SCALE_DESC] = "The tilt the keyboard's sticks get while the walk key is held. Half is a walk, the "
                                "full tilt a run.",

    [UI_BINDS_HELP] = "Select a cell and press A or Enter, then the key, mouse button or controller button. Delete or "
                      "X clears a cell. A key in use moves here; a controller button swaps with the one it replaces.",
    [UI_BINDS_COL_KEY1] = "Key 1", [UI_BINDS_COL_KEY2] = "Key 2", [UI_BINDS_COL_PAD] = "Controller",
    [UI_BINDS_GROUP_BUTTONS] = "Buttons", [UI_BINDS_GROUP_MOVE] = "Movement", [UI_BINDS_GROUP_CAMERA] = "Camera",
    [UI_BINDS_PRESS_KEY] = "Press a key or mouse button for %s",
    [UI_BINDS_PRESS_PAD] = "Press a controller button for %s",
    [UI_BINDS_CANCEL_HINT] = "Esc cancels, or wait %d s",
    [UI_BINDS_TAKEN] = "%s was taken from %s", [UI_BINDS_SWAPPED] = "%s swapped with %s",
    [UI_BINDS_RESET] = "Reset all buttons",
    [UI_BINDS_RESET_DESC] = "Back to the default keys and controller buttons. The button layout above applies again: remapped controller buttons stay where you put them until this reset.",
    [UI_BIND_A] = "A", [UI_BIND_B] = "B", [UI_BIND_X] = "X", [UI_BIND_Y] = "Y",
    [UI_BIND_LCLICK] = "Left stick click", [UI_BIND_RCLICK] = "Right stick click", [UI_BIND_L] = "L",
    [UI_BIND_R] = "R", [UI_BIND_ZL] = "ZL", [UI_BIND_ZR] = "ZR", [UI_BIND_PLUS] = "Plus", [UI_BIND_MINUS] = "Minus",
    [UI_BIND_LEFT] = "D-pad left", [UI_BIND_UP] = "D-pad up", [UI_BIND_RIGHT] = "D-pad right",
    [UI_BIND_DOWN] = "D-pad down",
    [UI_BIND_LSTICK_LEFT] = "Move left", [UI_BIND_LSTICK_UP] = "Move forward", [UI_BIND_LSTICK_RIGHT] = "Move right",
    [UI_BIND_LSTICK_DOWN] = "Move back",
    [UI_BIND_RSTICK_LEFT] = "Camera left", [UI_BIND_RSTICK_UP] = "Camera up", [UI_BIND_RSTICK_RIGHT] = "Camera right",
    [UI_BIND_RSTICK_DOWN] = "Camera down",
    [UI_BIND_DESC_A] = "Interact, pick up, talk.", [UI_BIND_DESC_B] = "Run, cancel, go back.",
    [UI_BIND_DESC_X] = "Jump.", [UI_BIND_DESC_Y] = "Attack.", [UI_BIND_DESC_LCLICK] = "Crouch.",
    [UI_BIND_DESC_RCLICK] = "First-person view.", [UI_BIND_DESC_L] = "Abilities.",
    [UI_BIND_DESC_R] = "Throw a weapon.", [UI_BIND_DESC_ZL] = "Shield, lock on.", [UI_BIND_DESC_ZR] = "Bow.",
    [UI_BIND_DESC_PLUS] = "Menu, inventory.", [UI_BIND_DESC_MINUS] = "Map.",
    [UI_BIND_DESC_DPAD] = "Quick-select.",
    [UI_BIND_DESC_MOVE] = "Moves Link like the left stick. The controller's left stick always works too.",
    [UI_BIND_DESC_CAMERA] = "Turns the camera like the right stick. The mouse and the controller's right stick always "
                            "work too.",
    [UI_KEY_SPACE] = "Space", [UI_KEY_TAB] = "Tab", [UI_KEY_LSHIFT] = "Left Shift", [UI_KEY_LCTRL] = "Left Ctrl",
    [UI_KEY_UP] = "Up arrow", [UI_KEY_DOWN] = "Down arrow", [UI_KEY_LEFT] = "Left arrow",
    [UI_KEY_RIGHT] = "Right arrow",
    [UI_KEY_MOUSE_L] = "Left mouse", [UI_KEY_MOUSE_R] = "Right mouse", [UI_KEY_MOUSE_M] = "Middle mouse",
    [UI_KEY_MOUSE_X1] = "Mouse 4", [UI_KEY_MOUSE_X2] = "Mouse 5",
    [UI_KEY_ENTER] = "Enter", [UI_KEY_BACKSPACE] = "Backspace", [UI_KEY_RSHIFT] = "Right Shift",
    [UI_KEY_RCTRL] = "Right Ctrl", [UI_KEY_LALT] = "Alt", [UI_KEY_CAPSLOCK] = "Caps Lock", [UI_KEY_INSERT] = "Insert",
    [UI_KEY_DELETE] = "Delete", [UI_KEY_HOME] = "Home", [UI_KEY_END] = "End", [UI_KEY_PAGEUP] = "Page Up",
    [UI_KEY_PAGEDOWN] = "Page Down", [UI_KEY_NUM_FMT] = "Num %s",
    [UI_PAD_SOUTH] = "Bottom button", [UI_PAD_EAST] = "Right button", [UI_PAD_WEST] = "Left button",
    [UI_PAD_NORTH] = "Top button", [UI_PAD_LSTICK] = "Left stick", [UI_PAD_RSTICK] = "Right stick",
    [UI_PAD_L] = "Left bumper", [UI_PAD_R] = "Right bumper", [UI_PAD_START] = "Start", [UI_PAD_BACK] = "Back",
    [UI_PAD_DPAD_LEFT] = "D-pad left", [UI_PAD_DPAD_UP] = "D-pad up", [UI_PAD_DPAD_RIGHT] = "D-pad right",
    [UI_PAD_DPAD_DOWN] = "D-pad down", [UI_PAD_ZL] = "Left trigger", [UI_PAD_ZR] = "Right trigger",
    [UI_PAD_LPADDLE1] = "Left back button", [UI_PAD_RPADDLE1] = "Right back button",
    [UI_PAD_LPADDLE2] = "Left back button 2", [UI_PAD_RPADDLE2] = "Right back button 2",
    [UI_PAD_CROSS] = "Cross", [UI_PAD_CIRCLE] = "Circle", [UI_PAD_SQUARE] = "Square", [UI_PAD_TRIANGLE] = "Triangle",

    [UI_MENU_AMIIBO_DIR] = "Amiibo folder",
    [UI_MENU_AMIIBO_OPEN] = "Open",
    [UI_MENU_AMIIBO_DEFAULT_DIR] = "Default folder",
    [UI_MENU_AMIIBO_REFRESH] = "Refresh",
    [UI_MENU_AMIIBO_PICK] = "Amiibo dumps (540-byte .bin or .nfc files):",
    [UI_MENU_AMIIBO_NO_DIR] = "Cannot open the folder %s",
    [UI_MENU_AMIIBO_READER] = "NFC reader",
    [UI_MENU_AMIIBO_STATE] = "Reader:",
    [UI_MENU_AMIIBO_STATE_IDLE] = "ready",
    [UI_MENU_AMIIBO_STATE_SEARCHING] = "the game is waiting for an amiibo",
    [UI_MENU_AMIIBO_STATE_FOUND] = "amiibo on the reader",
    [UI_MENU_AMIIBO_STATE_MOUNTED] = "the game is reading the amiibo",
    [UI_MENU_AMIIBO_STATE_OFF] = "off",
    [UI_MENU_AMIIBO_FILE] = "Selected file:",
    [UI_MENU_AMIIBO_NO_FILE] = "(no file selected)",
    [UI_MENU_AMIIBO_TAP] = "Tap the amiibo",
    [UI_MENU_AMIIBO_REMOVE] = "Lift the amiibo",
    [UI_MENU_AMIIBO_AUTO_REMOVE] = "Lift the amiibo by itself after a tap",
    [UI_MENU_AMIIBO_HOLD] = "Time on the reader",
    [UI_MENU_AMIIBO_SECONDS] = "%.1f s",
    [UI_MENU_AMIIBO_TAG] = "Amiibo on the reader:",
    [UI_MENU_AMIIBO_NO_TAG] = "No amiibo on the reader.",
    [UI_MENU_AMIIBO_TAG_FILE] = "File: %s",
    [UI_MENU_AMIIBO_TAG_ID] = "Character %s, series %02x, number %04x, type %02x",
    [UI_MENU_AMIIBO_TAG_UID] = "UID: %s",
    [UI_MENU_AMIIBO_READY] = "Ready",
    [UI_MENU_AMIIBO_TAPPED] = "Tapped: %s",
    [UI_MENU_AMIIBO_LOAD_FAILED] = "Not an amiibo dump (540-byte NTAG215): %s",
    [UI_MENU_AMIIBO_REMOVED] = "Amiibo lifted",
    [UI_MENU_AMIIBO_STATUS] = "Status: %s",
    [UI_MENU_MENU_LANG] = "Menu language",
    [UI_MENU_MENU_LANG_DESC] = "Language of this menu and the performance overlay. Automatic follows the system's "
                               "language when the menu has it, else English. Applies at once.",
    [UI_MENU_MENU_LANG_AUTO] = "Automatic (system)",

    [UI_MENU_ADV_WARNING] = "These settings are for testing and troubleshooting. Change them only when you know why.",
    [UI_MENU_GROUP_SCHED] = "Scheduling", [UI_MENU_GROUP_SHADERS] = "Shaders and pipelines",
    [UI_MENU_GROUP_TEXMEM] = "Textures and memory", [UI_MENU_GROUP_FIXES] = "Render fixes",
    [UI_MENU_GROUP_VULKAN] = "Vulkan", [UI_MENU_GROUP_DIAG] = "Diagnostics",
    [UI_MENU_ASYNC_PREEMPT] = "Virtual core preemption",
    [UI_MENU_ASYNC_PREEMPT_DESC] = "Lets a game thread interrupt another one on the same virtual core.",
    [UI_MENU_PREEMPT_OFF] = "Off", [UI_MENU_PREEMPT_ON] = "On", [UI_MENU_PREEMPT_INHERIT] = "Inheritance only",
    [UI_MENU_REPLAY_ASYNC] = "GPU frame prep on its own thread",
    [UI_MENU_REPLAY_ASYNC_DESC] = "Prepares frames for the GPU on a separate thread instead of the game thread. Holds 60 fps in big fights, "
                                  "where the game thread alone falls to about 45. Adds up to one frame of latency.",
    [UI_MENU_TIMER_1MS] = "1 ms Windows timer",
    [UI_MENU_TIMER_1MS_DESC] = "Raises the system timer resolution to 1 ms for steadier frame pacing.",
    [UI_MENU_PIPE_WAIT] = "Waiting draw handling",
    [UI_MENU_PIPE_WAIT_DESC] = "Default keeps the current scheduling with a reserved worker for waiting draws; Old (draw) uses the previous scheduling.",
    [UI_MENU_PIPE_WAIT_DEFAULT] = "Default", [UI_MENU_PIPE_WAIT_DRAW] = "Old (draw)",
    [UI_MENU_PIPE_SKIP] = "Draws skipped while a pipeline builds",
    [UI_MENU_PIPE_SKIP_DESC] = "Which draws are dropped instead of waiting for their pipeline.",
    [UI_MENU_PIPE_SKIP_STRICT] = "Strict", [UI_MENU_PIPE_SKIP_ANY] = "Any",
    [UI_MENU_PIPE_SKIP_FULLSCREEN] = "Fullscreen passes",
    [UI_MENU_LIB_PREBUILD] = "Stage library prebuild",
    [UI_MENU_LIB_PREBUILD_DESC] = "Builds shader stage libraries ahead of time in the background.",
    [UI_MENU_LIB_PREBUILD_THREADS] = "Stage library prebuild threads",
    [UI_MENU_LIB_PREBUILD_THREADS_DESC] = "Threads used for the prebuild; Automatic picks a number from your CPU.",
    [UI_MENU_PIPE_PREBUILD] = "Pipeline prebuild",
    [UI_MENU_PIPE_PREBUILD_DESC] = "Builds known pipelines ahead of time.",
    [UI_MENU_PREWARM_XLATE] = "Translator prewarm",
    [UI_MENU_PREWARM_XLATE_DESC] = "Threads that translate known shaders at start; Automatic takes half the CPU threads, at most 16.",
    [UI_MENU_GPL] = "Graphics pipeline library",
    [UI_MENU_GPL_DESC] = "Uses the Vulkan pipeline library extension; turn off if your driver misbehaves with it.",
    [UI_MENU_ASTC] = "ASTC texture decoding", [UI_MENU_ASTC_DESC] = "Where compressed ASTC textures are decoded.",
    [UI_MENU_ASTC_AUTO] = "Automatic", [UI_MENU_ASTC_GPU] = "GPU shader", [UI_MENU_ASTC_NATIVE] = "Native",
    [UI_MENU_ASTC_CPU] = "CPU",
    [UI_MENU_MEMORY] = "GPU memory mode", [UI_MENU_MEMORY_DESC] = "How game memory is shared with the GPU.",
    [UI_MENU_MEMORY_AUTO] = "Automatic", [UI_MENU_MEMORY_IMPORT] = "Import", [UI_MENU_MEMORY_MIRROR] = "Mirror",
    [UI_MENU_MEMORY_VERIFY] = "Verify",
    [UI_MENU_TEX_ALIAS] = "Texture aliasing", [UI_MENU_TEX_ALIAS_DESC] = "A new texture created over a live texture's memory starts with that content instead of empty.",
    [UI_MENU_TEX_VRAM_PRESSURE] = "Texture memory limit", [UI_MENU_TEX_VRAM_PRESSURE_DESC] = "Percent of video memory above which textures the game released are dropped (they come back from storage when used again). 0 keeps them.",
    [UI_MENU_ASTC_BC] = "Compress ASTC to BC3", [UI_MENU_ASTC_BC_DESC] = "With the GPU decoder: re-encodes decoded ASTC textures to BC3, a quarter of the video memory. For GPUs with 4 GB or less; textures look slightly blockier.",
    [UI_MENU_FIX_C0] = "Constant buffer fix",
    [UI_MENU_FIX_C0_DESC] = "Fills the driver-reserved constant bank c0 (storage sizes, dispatch size, point size, base instance) instead of reading zeros.",
    [UI_MENU_FIX_ZERO] = "Zero new GPU memory",
    [UI_MENU_FIX_ZERO_DESC] = "New GPU memory pools start zero-filled, as Switch memory does.",
    [UI_MENU_BIND_SKIP] = "Skip repeated binds",
    [UI_MENU_BIND_SKIP_DESC] = "Optimisation: skips bindings that did not change.",
    [UI_MENU_SHADER_MEMO] = "Shader memo", [UI_MENU_SHADER_MEMO_DESC] = "Optimisation: remembers shader lookups.",
    [UI_MENU_PIPE_MEMO] = "Pipeline memo", [UI_MENU_PIPE_MEMO_DESC] = "Optimisation: reuses the previous draw's pipeline when its state repeats.",
    [UI_MENU_TEX_MEMO] = "Texture binding memo", [UI_MENU_TEX_MEMO_DESC] = "Optimisation: remembers texture descriptors between draws.",
    [UI_MENU_DESC_TEMPLATE] = "Descriptor templates",
    [UI_MENU_DESC_TEMPLATE_DESC] = "Optimisation: updates descriptors in bulk.",
    [UI_MENU_PUSH_SKIP] = "Skip repeated push constants",
    [UI_MENU_PUSH_SKIP_DESC] = "Optimisation: skips push constants that did not change.",
    [UI_MENU_SPLIT_DRAWS] = "Early GPU submission",
    [UI_MENU_SPLIT_DRAWS_DESC] = "Submits GPU work in parts every N draws between passes so the GPU starts earlier; 0 turns it off.",
    [UI_MENU_VK_VALIDATION] = "Validation layer",
    [UI_MENU_VK_VALIDATION_DESC] = "Checks every Vulkan call and slows the game down a lot.",
    [UI_MENU_VK_VALIDATION_MISSING] = "The validation layer is not installed.",
    [UI_MENU_DLAA_MISSING] = "DLSS is not available: it needs an NVIDIA RTX card and nvngx_dlss.dll, which the Windows build downloads. Build again, or put the file next to AgeOfCalamityRecomp.exe, and restart. Or set the upscaler to Automatic or AMD FSR 3.1.",
    [UI_MENU_VK_DYN_VERTEX] = "Dynamic vertex state",
    [UI_MENU_VK_DYN_VERTEX_DESC] = "Forces dynamic vertex input on or off; the default depends on the GPU.",
    [UI_MENU_VK_DYN_A2C] = "Dynamic alpha to coverage",
    [UI_MENU_VK_DYN_A2C_DESC] = "Forces dynamic alpha to coverage on or off. Default: off on AMD's driver for RDNA 2 and "
                                "older, which draws characters black with it, on elsewhere.",
    [UI_MENU_VK_SUBGROUP_PIN] = "Pin subgroup size", [UI_MENU_VK_SUBGROUP_PIN_DESC] = "Fixes the shader subgroup size.",
    [UI_MENU_TRACE] = "Performance log", [UI_MENU_TRACE_DESC] = "Writes frame timings to a file.",
    [UI_MENU_FLIGHT] = "Flight recorder",
    [UI_MENU_FLIGHT_DESC] = "Keeps a short history and saves it when a frame freezes.",
    [UI_MENU_FLIGHT_SPIKE_MS] = "Freeze threshold (ms)",
    [UI_MENU_FLIGHT_SPIKE_MS_DESC] = "Frame time that counts as a freeze.",
    [UI_MENU_INPUT_RECORD] = "Input recording",
    [UI_MENU_INPUT_RECORD_DESC] = "Writes your inputs to a file for bug reports.",
    [UI_MENU_OPEN_LOG] = "Open log folder", [UI_MENU_OPEN_LOG_DESC] = "Shows the folder with the log files.",
    [UI_MENU_COPY_INFO] = "Copy system info",
    [UI_MENU_COPY_INFO_DESC] = "Copies GPU, driver, CPU, RAM and changed settings to the clipboard.",
    [UI_MENU_COPY_INFO_DONE] = "Copied", [UI_MENU_ACTION_FAILED] = "did not work",
    [UI_MENU_RESET] = "Reset all settings to defaults", [UI_MENU_RESET_DESC] = "Applies to every tab.",
    [UI_MENU_RESET_CONFIRM] = "Reset every setting to its default?", [UI_MENU_RESET_YES] = "Reset",
    [UI_MENU_RESET_NO] = "Cancel",

    [UI_MENU_OV_FPS] = "%.0f FPS", [UI_MENU_OV_FRAME_MS] = "%.1f ms", [UI_MENU_OV_GAME_PCT] = "game %.0f%%",
    [UI_MENU_OV_MAX_MS] = "max %.1f ms",
    [UI_MENU_OV_GPU] = "GPU", [UI_MENU_OV_CPU] = "CPU", [UI_MENU_OV_RAM] = "RAM", [UI_MENU_OV_VRAM] = "VRAM",
    [UI_MENU_OV_NONE] = "-",
    [UI_MENU_OV_REPLAY] = "GPU frame prep", [UI_MENU_OV_GPU_TIME] = "GPU busy per frame",
    [UI_MENU_OV_PIPE_WAIT] = "Pipeline waits",
    [UI_MENU_OV_SHADER_WAIT] = "Shader waits", [UI_MENU_OV_TEX_UPLOADS] = "Texture uploads",
    [UI_MENU_OV_FMT_PCT] = "%.0f%%", [UI_MENU_OV_FMT_TEMP] = "%.0f °C", [UI_MENU_OV_FMT_POWER] = "%.0f W",
    [UI_MENU_OV_FMT_MHZ] = "%.0f MHz", [UI_MENU_OV_FMT_GB] = "%.1f GB", [UI_MENU_OV_FMT_GB_OF] = "/ %.1f GB",
    [UI_MENU_OV_GAME_GB] = "game %.1f GB", [UI_MENU_OV_FMT_MS] = "%.2f ms", [UI_MENU_OV_PER_FRAME] = "%.1f per frame",
    [UI_MENU_OV_WINDOW] = "Window", [UI_MENU_OV_OUTPUT] = "Game image", [UI_MENU_OV_SCENE] = "3D scene",
    [UI_MENU_OV_SCALING] = "Scaling", [UI_MENU_OV_LIMIT] = "Frame limit", [UI_MENU_OV_PRESENT] = "Presentation",
    [UI_MENU_OV_FMT_SIZE] = "%ux%u", [UI_MENU_OV_FMT_SCENE] = "%ux%u, %.0f%%",
    [UI_MENU_OV_FMT_FSR] = "AMD FSR 1, sharpness %d%%", [UI_MENU_OV_SCALE_NONE] = "none (1:1)",
    [UI_MENU_OV_SCALE_BILINEAR] = "bilinear",
    [UI_MENU_OV_SCALE_DOWN] = "bilinear (FSR only enlarges)",
    [UI_MENU_TAB_CHEATS] = "Cheats",
    [UI_MENU_CHEATS_NONE] = "No cheats for this game version. A mod's cheats are an Atmosphere cheat file in its "
                            "cheats folder, named after the game's build: mods\\<mod>\\cheats\\%s. Restart the game to "
                            "read them. For cheats that change the game's code, the game rebuilds itself at start (a "
                            "few minutes).",
    [UI_MENU_CHEATS_MOD] = "%s: %d cheats",
    [UI_MENU_CHEAT_CODE_DESC] = "Changes the game's code. Switches at once.",
    [UI_MENU_CHEAT_KEY_DESC] = "Changes the game's code while you hold %s. Switches at once.",
    [UI_MENU_CHEAT_DATA_DESC] = "Writes the game's memory every frame while it is on.",
    [UI_MENU_CHEAT_MASTER] = "always on",
    [UI_MENU_CHEAT_MASTER_DESC] = "The file's master code: it is always on.",
    [UI_MENU_CHEAT_RESTORE] = "not needed",
    [UI_MENU_CHEAT_RESTORE_DESC] = "It only writes the game's own code back. Switching a cheat off already does that.",
    [UI_MENU_CHEAT_REBUILD] = "after the rebuild",
    [UI_MENU_CHEAT_REBUILD_DESC] = "This cheat changes the game's code and is new or edited since the game was built. "
                                   "The game builds it in when it starts next (a few minutes), or use Rebuild the game "
                                   "now.",
    [UI_MENU_CHEAT_UNSUPPORTED] = "not supported",
    [UI_MENU_CHEAT_UNSUPPORTED_DESC] = "This cheat cannot be used with this version of the game:",
    [UI_MENU_CHEATS_REBUILD] = "Rebuild the game now",
    [UI_MENU_CHEATS_REBUILD_DESC] = "Some cheats change the game's code and are not built into it: they are new or "
                                    "edited, or their last rebuild failed or was cancelled. Closes the game; the setup "
                                    "builds them in (a few minutes) and starts the game again. Progress since your "
                                    "last save is lost.",
    [UI_MENU_MODS_EXEFS_UNSUPPORTED] = "This mod patches the game code (exefs) in a way the port cannot apply, so the "
                                       "whole mod stays off. %s",
    [UI_MENU_MODS_EXEFS_OTHER] = "Its code patches (exefs) are made for another game version.",
    [UI_MENU_MODS_EXEFS_ON] = "Code patches in use: %d files.",
    [UI_MENU_MODS_REPLACES_BUILTIN] = "Code patches in use: %d files. They replace the port's own patch %s, which "
                                      "stays off while this mod is on.",
};

// French: the keyboard, and the whole menu and overlay (menu language "fr").
static const char *const text_fr[UI_TEXT_COUNT] = {
    [UI_MENU_QUICK_TITLE] = "Age of Calamity Recomp  ·  menu rapide",
    [UI_MENU_QUICK_HINT] = "Maintenez L3+R3 pour ouvrir ou fermer. B ferme.",
    [UI_MENU_QUICK_FPS] = "Images par seconde",
    [UI_MENU_QUICK_FPS_DESC] = "30 FPS est la cadence d'origine du jeu ; 60 FPS affiche deux fois plus d'images à la même vitesse de jeu et demande environ deux fois plus de temps processeur ; 120, 144 et 240 FPS sont expérimentaux (en développement, pas encore optimisés) et demandent un écran aussi rapide. Pour économiser la batterie, restez à 30 et baissez aussi le TDP dans le menu performances de Steam.",
    [UI_MENU_QUICK_RES] = "Mode de la console",
    [UI_MENU_QUICK_RES_DESC] = "Si le jeu croit la Switch sur sa station (TV) ou en mode portable, comme quand vous la posez sur la station. Age of Calamity dessine à la même taille dans les deux : la taille de l'image est la Résolution du jeu du menu complet.",
    [UI_MENU_QUICK_RES_HANDHELD] = "Portable",
    [UI_MENU_QUICK_RES_DOCKED] = "TV (station)",
    [UI_MENU_QUICK_FULL] = "Menu complet",
    [UI_MENU_QUICK_MARK] = "Marquer ce moment",
    SWKBD("Saisie de texte", "Entrez un nom", "Cheval", "OK", "Annuler", "Manette : A = OK, B = Annuler"),

    [UI_MENU_TITLE] = "Age of Calamity Recomp",
    [UI_MENU_TAB_SCREEN] = "Écran", [UI_MENU_TAB_GAME] = "Jeu", [UI_MENU_TAB_MODS] = "Mods", [UI_MENU_TAB_SOUND] = "Son",
    [UI_MENU_TAB_CONTROLS] = "Commandes", [UI_MENU_TAB_AMIIBO] = "Amiibo", [UI_MENU_TAB_ADVANCED] = "Avancé",
    [UI_MENU_TAB_LANGUAGE] = "Langue",
    [UI_MENU_CONTINUE] = "Continuer", [UI_MENU_QUIT] = "Quitter le jeu",
    [UI_MENU_ON] = "Activé", [UI_MENU_OFF] = "Désactivé", [UI_MENU_AUTO] = "Automatique",
    [UI_MENU_DEFAULT_MARK] = "par défaut",
    [UI_MENU_AFTER_RESTART] = "après redémarrage", [UI_MENU_SET_BY_ENV] = "fixé par une variable d'environnement",
    [UI_MENU_SAVE_FAILED] = "impossible d'enregistrer les réglages",
    [UI_MENU_VERSION] = "Réglages  ·  version %s  ·  jeu %s",

    [UI_MENU_FULLSCREEN] = "Plein écran", [UI_MENU_FULLSCREEN_DESC] = "Le jeu occupe tout l'écran ; F11 bascule.",
    [UI_MENU_WINDOW_SIZE] = "Taille de la fenêtre",
    [UI_MENU_WINDOW_SIZE_DESC] = "Taille de la fenêtre du jeu hors plein écran.",
    [UI_MENU_PRESENT] = "Présentation", [UI_MENU_PRESENT_DESC] = "Comment les images terminées arrivent à l'écran.",
    [UI_MENU_PRESENT_MAILBOX] = "Mailbox", [UI_MENU_PRESENT_VSYNC] = "VSync", [UI_MENU_PRESENT_IMMEDIATE] = "Immédiate",
    [UI_MENU_PRESENT_ADAPTIVE] = "VSync adaptative",
    [UI_MENU_PRESENT_IN_USE] = "utilisé : %s",
    [UI_MENU_GPU] = "Carte graphique", [UI_MENU_GPU_DESC] = "Le périphérique Vulkan qui dessine le jeu.",
    [UI_MENU_GPU_AUTO] = "Automatique",
    [UI_MENU_PERF_OVERLAY] = "Affichage des performances",
    [UI_MENU_PERF_OVERLAY_DESC] = "Images par seconde en haut à droite. Complet ajoute les temps d'image avec un graphique, "
                                  "la charge du processeur, du GPU et de la mémoire, les températures et les attentes du "
                                  "thread de rendu. F10 fait défiler les modes.",
    [UI_MENU_PERF_OFF] = "Désactivé", [UI_MENU_PERF_FPS] = "FPS", [UI_MENU_PERF_FULL] = "Complet",
    [UI_PERF_LINE_FPS] = "%.0f fps",
    [UI_MENU_GRAPHICS] = "Image",
    [UI_MENU_OUTPUT_RES] = "Résolution du jeu",
    [UI_MENU_OUTPUT_RES_DESC] = "La taille à laquelle le jeu dessine son image et son interface ; 1920 x 1080 est celle "
                                "d'origine. 1280 x 720 correspond à l'écran du Steam Deck et soulage le GPU ; sous "
                                "1920 x 1080, la scène 3D peut monter jusqu'à 85 %. 2560 x 1440 et 3840 x 2160 affichent "
                                "une interface nette sur les grands écrans, pour plus de travail GPU.",
    [UI_MENU_RENDER_SCALE] = "Échelle du rendu 3D",
    [UI_MENU_RENDER_SCALE_DESC] = "Taille de la scène 3D par rapport à la résolution du jeu ; le FSR du jeu la ramène à "
                                  "cette taille. 100 % la dessine en taille réelle ; de 125 à 200 %, elle est dessinée plus "
                                  "grande puis réduite (suréchantillonnage) : le plus net, pour environ deux fois plus de "
                                  "travail GPU à 140 % et quatre fois plus à 200 %. Sous la résolution de jeu 1920 x 1080, "
                                  "elle s'arrête à 85 %, et la scène garde au plus 3840 x 2160 pixels (200 % de "
                                  "1920 x 1080, 100 % de 3840 x 2160).",
    [UI_MENU_RENDER_SCALE_GAME] = "Jeu (75 %)",
    [UI_MENU_DYNAMIC_RES] = "Résolution dynamique",
    [UI_MENU_DYNAMIC_RES_DESC] = "Dans les combats chargés, le jeu dessine un moment la scène 3D plus petite pour garder sa "
                                 "cadence (jusqu'à 70 % de sa hauteur lors d'une grande charge dans la plaine d'Hyrule). "
                                 "Désactivé garde toujours la scène à la taille de l'échelle du rendu 3D : plus net dans "
                                 "ces moments, pour plus de travail GPU. S'applique tout de suite.",
    [UI_MENU_DYNAMIC_RES_GAME] = "Jeu", [UI_MENU_DYNAMIC_RES_OFF] = "Désactivée (taille réelle)",
    [UI_MENU_UPSCALER] = "Mise à l'échelle de la fenêtre",
    [UI_MENU_UPSCALER_DESC] = "Comment l'image est agrandie quand la fenêtre dépasse la résolution du jeu. FSR garde les "
                              "contours et les détails plus nets que le bilinéaire.",
    [UI_MENU_UPSCALER_FSR] = "AMD FSR 1", [UI_MENU_UPSCALER_BILINEAR] = "Bilinéaire",
    [UI_MENU_ASPECT] = "Forme de l'image",
    [UI_MENU_ASPECT_DESC] = "Quand la forme de la fenêtre diffère de celle de la résolution du jeu (une image 16:9 sur un "
                            "écran 21:9) : garder la forme de l'image avec des bandes noires, ou l'étirer sur la fenêtre.",
    [UI_MENU_ASPECT_FIT] = "Bandes noires", [UI_MENU_ASPECT_STRETCH] = "Étirer",
    [UI_MENU_ANISO] = "Filtrage des textures",
    [UI_MENU_ANISO_DESC] = "Filtrage anisotrope des textures : le sol, les routes et les murs vus en biais restent nets au loin au lieu de devenir flous. Jeu : le niveau demandé par le jeu, comme sur la Switch.",
    [UI_MENU_ANISO_GAME] = "Jeu",
    [UI_MENU_SHARPNESS] = "Netteté FSR",
    [UI_MENU_SHARPNESS_DESC] = "Accentuation après la mise à l'échelle FSR ; 0 la désactive.",
    [UI_MENU_COLOR] = "Couleur",
    [UI_MENU_COLOR_PRESET] = "Profil de couleur",
    [UI_MENU_COLOR_PRESET_DESC] = "Règle les quatre valeurs ci-dessous. Vif : plus de contraste et de saturation, ombres un "
                                  "peu éclaircies (fait pour les écrans OLED). Clarté : un peu plus de contraste, de "
                                  "saturation et de luminosité. Modifier une valeur passe le profil en Personnalisé. "
                                  "L'interface du jeu change avec l'image ; ce menu, non.",
    [UI_MENU_COLOR_DEFAULT] = "D'origine", [UI_MENU_COLOR_VIVID] = "Vif (OLED)", [UI_MENU_COLOR_CLARITY] = "Clarté",
    [UI_MENU_COLOR_CUSTOM] = "Personnalisé",
    [UI_MENU_COLOR_BRIGHTNESS] = "Luminosité",
    [UI_MENU_COLOR_BRIGHTNESS_DESC] = "Ajoutée à chaque couleur ; 0 ne change rien.",
    [UI_MENU_COLOR_CONTRAST] = "Contraste",
    [UI_MENU_COLOR_CONTRAST_DESC] = "Écarte les couleurs du gris moyen (au-dessus de 100 %) ou les en rapproche ; 100 % ne "
                                    "change rien.",
    [UI_MENU_COLOR_SATURATION] = "Saturation",
    [UI_MENU_COLOR_SATURATION_DESC] = "Intensité des couleurs par rapport au gris ; 100 % ne change rien.",
    [UI_MENU_COLOR_GAMMA] = "Gamma",
    [UI_MENU_COLOR_GAMMA_DESC] = "Sous 1, éclaircit les tons sombres et moyens, au-dessus de 1 les assombrit ; 1 ne change "
                                 "rien.",
    [UI_MENU_HDR_GROUP] = "HDR",
    [UI_MENU_HDR] = "Sortie HDR",
    [UI_MENU_HDR_DESC] = "Des hautes lumières éclatantes sur un écran HDR. Demande le HDR activé dans Windows (\"Utiliser "
                         "le HDR\") ou un compositeur Wayland avec gestion des couleurs ; sinon l'image reste en SDR. "
                         "S'applique après un redémarrage. Le profil de couleur ci-dessus est pour le SDR et ne sert pas "
                         "en HDR.",
    [UI_MENU_HDR_PEAK] = "Luminosité maximale",
    [UI_MENU_HDR_PEAK_DESC] = "Le pic de votre écran en nits ; les hautes lumières les plus fortes s'arrêtent là.",
    [UI_MENU_HDR_GAME] = "Luminosité du jeu",
    [UI_MENU_HDR_GAME_DESC] = "Nits du blanc de l'image (blanc papier). 203 est la référence HDR ; montez-la dans une pièce "
                              "lumineuse.",
    [UI_MENU_HDR_UI] = "Luminosité de l'interface et du menu",
    [UI_MENU_HDR_UI_DESC] = "Nits du blanc de l'interface du jeu et de ce menu.",
    [UI_MENU_SHADOW_SCALE] = "Résolution des ombres",
    [UI_MENU_SHADOW_SCALE_DESC] = "Dessine la carte d'ombres du soleil autant de fois plus grande de chaque côté que les "
                                  "1024x2048 du jeu : contours d'ombre et ombres des feuillages plus nets. Prend effet au "
                                  "prochain lancement.",
    [UI_MENU_PCSS] = "Ombres douces",
    [UI_MENU_PCSS_DESC] = "Ombres du soleil nettes là où un objet touche le sol et plus douces à mesure que l'ombre "
                          "s'éloigne (PCSS). 0 garde le filtre du jeu. Prend effet au prochain lancement.",
    [UI_MENU_DLAA] = "DLSS / FSR 3.1",
    [UI_MENU_DLAA_DESC] = "Anticrénelage temporel et mise à l'échelle de la scène 3D : contours plus lisses et moins de "
                          "scintillement. NVIDIA DLSS sur les cartes RTX (nvngx_dlss.dll à côté de "
                          "AgeOfCalamityRecomp), AMD FSR 3.1 sur toute autre carte graphique (le moteur "
                          "ci-dessous). L'interface n'est pas touchée. Le mode ci-dessous fixe la résolution "
                          "de départ. Prend effet après un redémarrage.",
    [UI_MENU_DLSS] = "Mode",
    [UI_MENU_DLSS_DESC] = "Natif (DLAA) dessine la scène 3D en pleine résolution. Qualité, Équilibré et Performance la "
                          "dessinent à 67, 58 et 50 % et laissent DLSS ou FSR 3.1 l'agrandir, pour plus "
                          "d'images par seconde ; ils remplacent l'échelle du rendu 3D. Prend effet après un "
                          "redémarrage.",
    [UI_MENU_DLSS_DLAA] = "Natif (DLAA)", [UI_MENU_DLSS_QUALITY] = "Qualité", [UI_MENU_DLSS_BALANCED] = "Équilibré",
    [UI_MENU_DLSS_PERFORMANCE] = "Performance",
    [UI_MENU_DLSS_PRESET] = "Modèle DLSS",
    [UI_MENU_DLSS_PRESET_DESC] = "Le modèle DLSS utilisé (DLSS seulement). Automatique suit le conseil de NVIDIA pour "
                                 "chaque mode : K pour DLAA, Qualité et Équilibré, M pour Performance. J : un peu "
                                 "moins de traînées derrière les personnages en mouvement, un peu plus de "
                                 "scintillement. K : le modèle le plus net dans tous les modes. S'applique tout "
                                 "de suite.",
    [UI_MENU_DLSS_PRESET_DEFAULT] = "Automatique", [UI_MENU_DLSS_PRESET_K] = "K (le plus net)",
    [UI_MENU_DLSS_PRESET_J] = "J (moins de traînées)",
    [UI_MENU_FXAA] = "Anticrénelage (FXAA)",
    [UI_MENU_FXAA_DESC] = "Lisse les contours en escalier de la scène 3D ; l'interface reste nette. Adoucit un peu les détails fins. Désactivé quand DLAA ou DLSS est actif.",

    [UI_MENU_FPS_LIMIT] = "Images par seconde",
    [UI_MENU_FPS_LIMIT_DESC] = "30 FPS est la cadence d'origine du jeu. À 60 FPS, le jeu affiche deux fois plus d'images "
                               "et tourne toujours à la bonne vitesse ; il demande environ deux fois plus de temps "
                               "processeur, donc un combat chargé peut descendre sous 60. 120, 144 et 240 FPS demandent un "
                               "écran aussi rapide et beaucoup plus de temps processeur, et sont expérimentaux : encore en "
                               "développement, pas encore optimisés (les combats chargés restent sous 120 sur un "
                               "processeur rapide). S'applique tout de suite.",
    [UI_MENU_FPS_30] = "30 FPS", [UI_MENU_FPS_60] = "60 FPS", [UI_MENU_FPS_120] = "120 FPS (expérimental)",
    [UI_MENU_FPS_144] = "144 FPS (expérimental)", [UI_MENU_FPS_240] = "240 FPS (expérimental)",
    [UI_MENU_GAME_LANG] = "Langue du jeu",
    [UI_MENU_GAME_LANG_DESC] = "Langue des textes et des voix du jeu. Automatique suit la langue du système.",
    [UI_MENU_GAME_LANG_AUTO] = "Automatique",
    [UI_MENU_OPERATION_MODE] = "Mode de la console",
    [UI_MENU_OPERATION_MODE_DESC] = "Si le jeu se croit sur la station ou en mode portable.",
    [UI_MENU_MODE_DOCKED] = "Station (TV)", [UI_MENU_MODE_HANDHELD] = "Portable",
    [UI_MENU_FRAME_QUEUE] = "File d'images",
    [UI_MENU_FRAME_QUEUE_DESC] = "Le jeu commence une image pendant que la précédente attend son tour à l'écran : beaucoup "
                                 "moins d'images perdues à la limite, jusqu'à une image de latence en plus. S'applique "
                                 "tout de suite.",
    [UI_MENU_ASSUME_FOCUS] = "Continuer en arrière-plan",
    [UI_MENU_ASSUME_FOCUS_DESC] = "Activé (par défaut), le jeu continue pendant que vous êtes dans une autre fenêtre, et le "
                                  "clavier est ignoré jusqu'à ce que sa fenêtre revienne au premier plan. Désactivé, il se "
                                  "met en pause quand sa fenêtre perd le focus, comme sur la Switch avec le bouton HOME ; "
                                  "un chargement interrompu ainsi peut rester bloqué.",

    [UI_MENU_MODS_INSTALLED] = "Mods installés",
    [UI_MENU_MODS_NONE] = "Aucun mod pour l'instant. Décompressez un mod dans le dossier mods, un dossier par mod avec son "
                          "dossier romfs dedans, puis relancez le jeu. TKMM peut aussi y exporter ses mods fusionnés dans "
                          "un dossier.",
    [UI_MENU_MODS_OPEN] = "Ouvrir le dossier des mods",
    [UI_MENU_MODS_OPEN_DESC] = "Affiche le dossier d'où le jeu charge les mods. Il est créé s'il manque.",
    [UI_MENU_MODS_ROW_DESC] = "Fichiers du jeu remplacés : %u. Nouveaux fichiers ajoutés : %u.",
    [UI_MENU_MODS_OVERRIDDEN] = "%u de ses fichiers viennent de %s, placé plus haut dans la liste.",
    [UI_MENU_MODS_MOVE_HINT] = "Le bouton du haut ou Ctrl+Haut le monte, le bouton de gauche ou Ctrl+Bas le descend. "
                               "Quand deux mods modifient le même fichier, celui du haut l'emporte. Les changements "
                               "s'appliquent après un redémarrage.",
    [UI_MENU_MODS_EMPTY] = "Ce dossier ne contient ni romfs, ni exefs, ni fichier de triche.",
    [UI_MENU_MODS_SUBSDK] = "Ce mod apporte son propre code de jeu (un plugin subsdk ou un main remplacé). Le portage ne "
                            "peut pas charger de modules de code, donc tout le mod reste désactivé.",
    [UI_MENU_MODS_EXEFS_LATER] = "Ce mod modifie le code du jeu (exefs). Le jeu intègre ces modifications à son prochain "
                                 "lancement (une recompilation de quelques minutes) ; d'ici là, tout le mod reste "
                                 "désactivé.",
    [UI_MENU_MODS_CHEATS] = "Codes de triche : %d. Activez-les dans l'onglet Triche.",
    [UI_MENU_MODS_CHEATS_FILE] = "Les codes de triche de cette version du jeu sont lus au lancement dans cheats\\%s de ce "
                                 "mod.",
    [UI_MENU_MODS_BAD_NAME] = "Le nom du dossier contient |, que la liste des mods utilise entre les noms. Renommez le "
                              "dossier.",
    [UI_MENU_MODS_OTHER_VERSION] = "%u de ses fichiers sont faits pour une autre version du jeu et sont ignorés. Cherchez "
                                   "une version de ce mod faite pour votre version du jeu.",
    [UI_MENU_MODS_UNUSED] = "inutilisé",

    [UI_MENU_VOLUME] = "Volume", [UI_MENU_VOLUME_DESC] = "Volume général du jeu.",
    [UI_MENU_MUTE] = "Muet", [UI_MENU_MUTE_DESC] = "Coupe le son du jeu sans changer le volume.",
    [UI_MENU_MUTE_BG] = "Muet en arrière-plan",
    [UI_MENU_MUTE_BG_DESC] = "Coupe le son du jeu quand une autre fenêtre est active.",
    [UI_MENU_AUDIO_DEVICE] = "Périphérique de sortie",
    [UI_MENU_AUDIO_DEVICE_DESC] = "Où le son est joué. Système suit le périphérique par défaut de Windows. Un périphérique "
                                  "choisi puis débranché repasse sur celui du système jusqu'à son retour. S'applique tout "
                                  "de suite.",
    [UI_MENU_AUDIO_DEVICE_DEFAULT] = "Système (par défaut)",
    [UI_MENU_INVERT_LX] = "Inverser le stick gauche X", [UI_MENU_INVERT_LY] = "Inverser le stick gauche Y",
    [UI_MENU_INVERT_RX] = "Inverser le stick droit X (caméra)", [UI_MENU_INVERT_RY] = "Inverser le stick droit Y (caméra)",
    [UI_MENU_INVERT_DESC] = "Inverse cet axe du stick de la manette.",

    [UI_MENU_MOTION] = "Commandes par mouvement",
    [UI_MENU_MOTION_DESC] = "Viser avec le gyroscope de la manette.",
    [UI_MENU_RUMBLE_STRENGTH] = "Force des vibrations",
    [UI_MENU_RUMBLE_STRENGTH_DESC] = "L'intensité des vibrations de la manette.",
    [UI_MENU_RUMBLE_CURVE] = "Courbe des vibrations",
    [UI_MENU_RUMBLE_CURVE_DESC] = "Des valeurs plus basses renforcent les vibrations faibles.",
    [UI_MENU_RUMBLE_TRIGGERS] = "Vibration des gâchettes (Xbox)",
    [UI_MENU_RUMBLE_TRIGGERS_DESC] = "Fait aussi vibrer les gâchettes d'une manette Xbox.",
    [UI_MENU_PAD_LAYOUT] = "Disposition des boutons",
    [UI_MENU_PAD_LAYOUT_DESC] = "Quel bouton est A. Nintendo : celui de droite, comme sur une Switch (rond A, croix B, "
                                "carré Y, triangle X sur une manette PlayStation). Xbox : celui du bas, comme marqué sur "
                                "une manette Xbox. PlayStation : la croix, en bas, est A, le rond B, le carré X et le "
                                "triangle Y. Automatique choisit Nintendo pour les manettes Nintendo et PlayStation et Xbox "
                                "pour les autres. Icônes des boutons, plus bas, choisit les icônes affichées par le jeu.",
    [UI_MENU_PAD_LAYOUT_AUTO] = "Automatique", [UI_MENU_PAD_LAYOUT_NINTENDO] = "Nintendo (A à droite)",
    [UI_MENU_PAD_LAYOUT_XBOX] = "Xbox (A en bas)",
    [UI_MENU_PAD_LAYOUT_PLAYSTATION] = "PlayStation (croix = A)",
    [UI_MENU_PAD_LAYOUT_IN_USE] = "utilisé : %s",
    [UI_MENU_PROMPTS] = "Icônes des boutons",
    [UI_MENU_PROMPTS_DESC] = "La manette dont le jeu affiche les boutons. Nintendo : ceux du jeu. Xbox : LB, RB, LT, RT, "
                             "Menu et Affichage, et A, B, X et Y tels qu'ils sont marqués sur le bouton appuyé (A est sur "
                             "le bouton de droite comme sur la Switch, marqué B sur une manette Xbox, donc les icônes de A "
                             "montrent B). PlayStation : L1, R1, L2, R2, Options et Créer, et croix, rond, carré et "
                             "triangle de la même façon (A montre le rond). S'applique tout de suite. Elles font partie "
                             "des fichiers du jeu, donc un bouton réassigné plus bas garde son ancienne icône à l'écran.",
    [UI_MENU_PROMPTS_NINTENDO] = "Nintendo", [UI_MENU_PROMPTS_XBOX] = "Xbox",
    [UI_MENU_PROMPTS_PLAYSTATION] = "PlayStation",
    [UI_MENU_PROMPTS_MISSING] = "fichiers d'icônes absents : relancez le setup",
    [UI_MENU_PROMPTS_FAILED] = "fichiers d'icônes abîmés : relancez le setup",
    [UI_MENU_PROMPTS_RESTART] = "les icônes changent après redémarrage",
    [UI_MENU_RESTART] = "Redémarrer maintenant",
    [UI_MENU_RESTART_DESC] = "Ferme le jeu et le relance avec les nouvelles icônes. La progression depuis votre dernière "
                             "sauvegarde est perdue.",
    [UI_MENU_RESTART_CONFIRM] = "Redémarrer le jeu maintenant ? La progression depuis votre dernière sauvegarde est perdue.",
    [UI_MENU_RESTART_YES] = "Redémarrer",
    [UI_MENU_RESTART_SAVING] = "Le jeu sauvegarde : le redémarrage sera possible dans un instant.",
    [UI_MENU_LSTICK_DEADZONE] = "Zone morte du stick gauche", [UI_MENU_RSTICK_DEADZONE] = "Zone morte du stick droit",
    [UI_MENU_STICK_DEADZONE_DESC] = "De combien un stick de manette doit bouger avant que le jeu le voie. Augmentez-la si le personnage ou la caméra bouge tout seul : lâchez le stick, lisez \"stick actuel\" et réglez la zone morte quelques pour cent au-dessus.",
    [UI_MENU_STICK_DEADZONE_NOW] = "stick actuel : %.0f %%",
    [UI_MENU_MOUSE_SENS] = "Sensibilité de la caméra à la souris",
    [UI_MENU_MOUSE_SENS_DESC] = "La vitesse à laquelle la souris tourne la caméra.",
    [UI_MENU_WALK_KEY] = "Touche de marche",
    [UI_MENU_WALK_KEY_DESC] = "Maintenez-la pour n'incliner qu'en partie les sticks du clavier : le clavier marche au lieu "
                              "de courir. L'échelle de marche règle de combien. La manette et la souris ne sont pas "
                              "concernées.",
    [UI_MENU_WALK_KEY_TAKEN] = "aussi %s",
    [UI_MENU_WALK_SCALE] = "Échelle de marche",
    [UI_MENU_WALK_SCALE_DESC] = "L'inclinaison des sticks du clavier tant que la touche de marche est maintenue. La moitié "
                                "fait marcher, l'inclinaison complète courir.",

    [UI_BINDS_HELP] = "Choisissez une case et appuyez sur A ou Entrée, puis sur la touche, le bouton de souris ou le bouton "
                      "de manette. Suppr ou X vide une case. Une touche déjà utilisée est déplacée ici ; un bouton de "
                      "manette est échangé avec celui qu'il remplace.",
    [UI_BINDS_COL_KEY1] = "Touche 1", [UI_BINDS_COL_KEY2] = "Touche 2", [UI_BINDS_COL_PAD] = "Manette",
    [UI_BINDS_GROUP_BUTTONS] = "Boutons", [UI_BINDS_GROUP_MOVE] = "Déplacement", [UI_BINDS_GROUP_CAMERA] = "Caméra",
    [UI_BINDS_PRESS_KEY] = "Appuyez sur une touche ou un bouton de souris pour %s",
    [UI_BINDS_PRESS_PAD] = "Appuyez sur un bouton de manette pour %s",
    [UI_BINDS_CANCEL_HINT] = "Échap annule, ou attendez %d s",
    [UI_BINDS_TAKEN] = "%s a été retiré de %s", [UI_BINDS_SWAPPED] = "%s échangé avec %s",
    [UI_BINDS_RESET] = "Réinitialiser tous les boutons",
    [UI_BINDS_RESET_DESC] = "Revient aux touches et boutons de manette par défaut. La disposition des boutons ci-dessus s'applique à nouveau : les boutons de manette réassignés restent où vous les avez mis jusqu'à cette réinitialisation.",
    [UI_BIND_A] = "A", [UI_BIND_B] = "B", [UI_BIND_X] = "X", [UI_BIND_Y] = "Y",
    [UI_BIND_LCLICK] = "Clic du stick gauche", [UI_BIND_RCLICK] = "Clic du stick droit", [UI_BIND_L] = "L",
    [UI_BIND_R] = "R", [UI_BIND_ZL] = "ZL", [UI_BIND_ZR] = "ZR", [UI_BIND_PLUS] = "Plus", [UI_BIND_MINUS] = "Moins",
    [UI_BIND_LEFT] = "Croix directionnelle gauche", [UI_BIND_UP] = "Croix directionnelle haut",
    [UI_BIND_RIGHT] = "Croix directionnelle droite", [UI_BIND_DOWN] = "Croix directionnelle bas",
    [UI_BIND_LSTICK_LEFT] = "Aller à gauche", [UI_BIND_LSTICK_UP] = "Avancer", [UI_BIND_LSTICK_RIGHT] = "Aller à droite",
    [UI_BIND_LSTICK_DOWN] = "Reculer",
    [UI_BIND_RSTICK_LEFT] = "Caméra à gauche", [UI_BIND_RSTICK_UP] = "Caméra vers le haut",
    [UI_BIND_RSTICK_RIGHT] = "Caméra à droite", [UI_BIND_RSTICK_DOWN] = "Caméra vers le bas",
    [UI_BIND_DESC_A] = "Interagir, ramasser, parler.", [UI_BIND_DESC_B] = "Courir, annuler, revenir.",
    [UI_BIND_DESC_X] = "Sauter.", [UI_BIND_DESC_Y] = "Attaquer.", [UI_BIND_DESC_LCLICK] = "S'accroupir.",
    [UI_BIND_DESC_RCLICK] = "Vue à la première personne.", [UI_BIND_DESC_L] = "Capacités.",
    [UI_BIND_DESC_R] = "Lancer une arme.", [UI_BIND_DESC_ZL] = "Bouclier, verrouillage.", [UI_BIND_DESC_ZR] = "Arc.",
    [UI_BIND_DESC_PLUS] = "Menu, inventaire.", [UI_BIND_DESC_MINUS] = "Carte.",
    [UI_BIND_DESC_DPAD] = "Sélection rapide.",
    [UI_BIND_DESC_MOVE] = "Déplace Link comme le stick gauche. Le stick gauche de la manette marche toujours aussi.",
    [UI_BIND_DESC_CAMERA] = "Tourne la caméra comme le stick droit. La souris et le stick droit de la manette marchent "
                            "toujours aussi.",
    [UI_KEY_SPACE] = "Espace", [UI_KEY_TAB] = "Tab", [UI_KEY_LSHIFT] = "Maj gauche", [UI_KEY_LCTRL] = "Ctrl gauche",
    [UI_KEY_UP] = "Flèche haut", [UI_KEY_DOWN] = "Flèche bas", [UI_KEY_LEFT] = "Flèche gauche",
    [UI_KEY_RIGHT] = "Flèche droite",
    [UI_KEY_MOUSE_L] = "Clic gauche", [UI_KEY_MOUSE_R] = "Clic droit", [UI_KEY_MOUSE_M] = "Clic molette",
    [UI_KEY_MOUSE_X1] = "Souris 4", [UI_KEY_MOUSE_X2] = "Souris 5",
    [UI_KEY_ENTER] = "Entrée", [UI_KEY_BACKSPACE] = "Retour arrière", [UI_KEY_RSHIFT] = "Maj droite",
    [UI_KEY_RCTRL] = "Ctrl droit", [UI_KEY_LALT] = "Alt", [UI_KEY_CAPSLOCK] = "Verr. Maj", [UI_KEY_INSERT] = "Inser",
    [UI_KEY_DELETE] = "Suppr", [UI_KEY_HOME] = "Début", [UI_KEY_END] = "Fin", [UI_KEY_PAGEUP] = "Page préc.",
    [UI_KEY_PAGEDOWN] = "Page suiv.", [UI_KEY_NUM_FMT] = "Pavé num. %s",
    [UI_PAD_SOUTH] = "Bouton du bas", [UI_PAD_EAST] = "Bouton de droite", [UI_PAD_WEST] = "Bouton de gauche",
    [UI_PAD_NORTH] = "Bouton du haut", [UI_PAD_LSTICK] = "Stick gauche", [UI_PAD_RSTICK] = "Stick droit",
    [UI_PAD_L] = "Bouton épaule gauche", [UI_PAD_R] = "Bouton épaule droit", [UI_PAD_START] = "Start",
    [UI_PAD_BACK] = "Back",
    [UI_PAD_DPAD_LEFT] = "Croix gauche", [UI_PAD_DPAD_UP] = "Croix haut", [UI_PAD_DPAD_RIGHT] = "Croix droite",
    [UI_PAD_DPAD_DOWN] = "Croix bas", [UI_PAD_ZL] = "Gâchette gauche", [UI_PAD_ZR] = "Gâchette droite",
    [UI_PAD_LPADDLE1] = "Palette arrière gauche", [UI_PAD_RPADDLE1] = "Palette arrière droite",
    [UI_PAD_LPADDLE2] = "Palette arrière gauche 2", [UI_PAD_RPADDLE2] = "Palette arrière droite 2",
    [UI_PAD_CROSS] = "Croix", [UI_PAD_CIRCLE] = "Rond", [UI_PAD_SQUARE] = "Carré", [UI_PAD_TRIANGLE] = "Triangle",

    [UI_MENU_AMIIBO_DIR] = "Dossier des amiibo",
    [UI_MENU_AMIIBO_OPEN] = "Ouvrir",
    [UI_MENU_AMIIBO_DEFAULT_DIR] = "Dossier par défaut",
    [UI_MENU_AMIIBO_REFRESH] = "Actualiser",
    [UI_MENU_AMIIBO_PICK] = "Copies d'amiibo (fichiers .bin ou .nfc de 540 octets) :",
    [UI_MENU_AMIIBO_NO_DIR] = "Impossible d'ouvrir le dossier %s",
    [UI_MENU_AMIIBO_READER] = "Lecteur NFC",
    [UI_MENU_AMIIBO_STATE] = "Lecteur :",
    [UI_MENU_AMIIBO_STATE_IDLE] = "prêt",
    [UI_MENU_AMIIBO_STATE_SEARCHING] = "le jeu attend un amiibo",
    [UI_MENU_AMIIBO_STATE_FOUND] = "amiibo sur le lecteur",
    [UI_MENU_AMIIBO_STATE_MOUNTED] = "le jeu lit l'amiibo",
    [UI_MENU_AMIIBO_STATE_OFF] = "éteint",
    [UI_MENU_AMIIBO_FILE] = "Fichier choisi :",
    [UI_MENU_AMIIBO_NO_FILE] = "(aucun fichier choisi)",
    [UI_MENU_AMIIBO_TAP] = "Poser l'amiibo",
    [UI_MENU_AMIIBO_REMOVE] = "Retirer l'amiibo",
    [UI_MENU_AMIIBO_AUTO_REMOVE] = "Retirer l'amiibo tout seul après l'avoir posé",
    [UI_MENU_AMIIBO_HOLD] = "Temps sur le lecteur",
    [UI_MENU_AMIIBO_SECONDS] = "%.1f s",
    [UI_MENU_AMIIBO_TAG] = "Amiibo sur le lecteur :",
    [UI_MENU_AMIIBO_NO_TAG] = "Aucun amiibo sur le lecteur.",
    [UI_MENU_AMIIBO_TAG_FILE] = "Fichier : %s",
    [UI_MENU_AMIIBO_TAG_ID] = "Personnage %s, série %02x, numéro %04x, type %02x",
    [UI_MENU_AMIIBO_TAG_UID] = "UID : %s",
    [UI_MENU_AMIIBO_READY] = "Prêt",
    [UI_MENU_AMIIBO_TAPPED] = "Posé : %s",
    [UI_MENU_AMIIBO_LOAD_FAILED] = "Pas une copie d'amiibo (NTAG215 de 540 octets) : %s",
    [UI_MENU_AMIIBO_REMOVED] = "Amiibo retiré",
    [UI_MENU_AMIIBO_STATUS] = "État : %s",
    [UI_MENU_MENU_LANG] = "Langue du menu",
    [UI_MENU_MENU_LANG_DESC] = "Langue de ce menu et de l'affichage des performances. Automatique suit la langue du "
                               "système quand le menu l'a, sinon l'anglais. S'applique tout de suite.",
    [UI_MENU_MENU_LANG_AUTO] = "Automatique (système)",

    [UI_MENU_ADV_WARNING] = "Ces réglages servent aux tests et au dépannage. Ne les changez que si vous savez pourquoi.",
    [UI_MENU_GROUP_SCHED] = "Ordonnancement", [UI_MENU_GROUP_SHADERS] = "Shaders et pipelines",
    [UI_MENU_GROUP_TEXMEM] = "Textures et mémoire", [UI_MENU_GROUP_FIXES] = "Corrections du rendu",
    [UI_MENU_GROUP_VULKAN] = "Vulkan", [UI_MENU_GROUP_DIAG] = "Diagnostic",
    [UI_MENU_ASYNC_PREEMPT] = "Préemption des cœurs virtuels",
    [UI_MENU_ASYNC_PREEMPT_DESC] = "Permet à un thread du jeu d'en interrompre un autre sur le même cœur virtuel.",
    [UI_MENU_PREEMPT_OFF] = "Désactivée", [UI_MENU_PREEMPT_ON] = "Activée", [UI_MENU_PREEMPT_INHERIT] = "Héritage seul",
    [UI_MENU_REPLAY_ASYNC] = "Préparation GPU sur son propre thread",
    [UI_MENU_REPLAY_ASYNC_DESC] = "Prépare les images pour le GPU sur un thread séparé au lieu du thread du jeu. Tient 60 "
                                  "fps dans les grands combats, où le thread du jeu seul tombe à environ 45. Ajoute "
                                  "jusqu'à une image de latence.",
    [UI_MENU_TIMER_1MS] = "Minuteur Windows à 1 ms",
    [UI_MENU_TIMER_1MS_DESC] = "Passe la résolution du minuteur système à 1 ms pour un rythme d'images plus régulier.",
    [UI_MENU_PIPE_WAIT] = "Gestion des draws en attente",
    [UI_MENU_PIPE_WAIT_DESC] = "Par défaut garde l'ordonnancement actuel avec un worker réservé aux draws en attente ; Ancien (draw) utilise l'ordonnancement précédent.",
    [UI_MENU_PIPE_WAIT_DEFAULT] = "Par défaut", [UI_MENU_PIPE_WAIT_DRAW] = "Ancien (draw)",
    [UI_MENU_PIPE_SKIP] = "Draws sautés pendant la création d'un pipeline",
    [UI_MENU_PIPE_SKIP_DESC] = "Quels draws sont abandonnés au lieu d'attendre leur pipeline.",
    [UI_MENU_PIPE_SKIP_STRICT] = "Strict", [UI_MENU_PIPE_SKIP_ANY] = "Tous",
    [UI_MENU_PIPE_SKIP_FULLSCREEN] = "Passes plein écran",
    [UI_MENU_LIB_PREBUILD] = "Précompilation des bibliothèques d'étapes",
    [UI_MENU_LIB_PREBUILD_DESC] = "Compile à l'avance, en arrière-plan, les bibliothèques d'étapes des shaders.",
    [UI_MENU_LIB_PREBUILD_THREADS] = "Threads de précompilation",
    [UI_MENU_LIB_PREBUILD_THREADS_DESC] = "Threads utilisés pour la précompilation ; Automatique choisit selon votre "
                                          "processeur.",
    [UI_MENU_PIPE_PREBUILD] = "Précompilation des pipelines",
    [UI_MENU_PIPE_PREBUILD_DESC] = "Compile à l'avance les pipelines connus.",
    [UI_MENU_PREWARM_XLATE] = "Préchauffage du traducteur",
    [UI_MENU_PREWARM_XLATE_DESC] = "Threads qui traduisent les shaders connus au lancement ; Automatique prend la moitié des threads du processeur, 16 au plus.",
    [UI_MENU_GPL] = "Graphics pipeline library",
    [UI_MENU_GPL_DESC] = "Utilise l'extension Vulkan de bibliothèques de pipelines ; désactivez-la si votre pilote se comporte mal avec.",
    [UI_MENU_ASTC] = "Décodage des textures ASTC", [UI_MENU_ASTC_DESC] = "Où sont décodées les textures compressées en ASTC.",
    [UI_MENU_ASTC_AUTO] = "Automatique", [UI_MENU_ASTC_GPU] = "Shader GPU", [UI_MENU_ASTC_NATIVE] = "Natif",
    [UI_MENU_ASTC_CPU] = "Processeur",
    [UI_MENU_MEMORY] = "Mode de mémoire GPU", [UI_MENU_MEMORY_DESC] = "Comment la mémoire du jeu est partagée avec le GPU.",
    [UI_MENU_MEMORY_AUTO] = "Automatique", [UI_MENU_MEMORY_IMPORT] = "Import", [UI_MENU_MEMORY_MIRROR] = "Miroir",
    [UI_MENU_MEMORY_VERIFY] = "Vérification",
    [UI_MENU_TEX_ALIAS] = "Alias de textures", [UI_MENU_TEX_ALIAS_DESC] = "Une nouvelle texture créée sur la mémoire d'une texture encore utilisée commence avec son contenu au lieu d'être vide.",
    [UI_MENU_TEX_VRAM_PRESSURE] = "Limite de mémoire des textures", [UI_MENU_TEX_VRAM_PRESSURE_DESC] = "Pourcentage de mémoire vidéo au-delà duquel les textures libérées par le jeu sont abandonnées (elles reviennent du disque quand elles resservent). 0 les garde.",
    [UI_MENU_ASTC_BC] = "Compresser l'ASTC en BC3", [UI_MENU_ASTC_BC_DESC] = "Avec le décodeur GPU : réencode les textures ASTC décodées en BC3, un quart de la mémoire vidéo. Pour les GPU de 4 Go ou moins ; les textures paraissent un peu plus pixelisées.",
    [UI_MENU_FIX_C0] = "Correction du constant buffer",
    [UI_MENU_FIX_C0_DESC] = "Remplit la banque de constantes c0 réservée au pilote (tailles des buffers, taille de dispatch, taille des points, instance de base) au lieu de lire des zéros.",
    [UI_MENU_FIX_ZERO] = "Mettre à zéro la nouvelle mémoire GPU",
    [UI_MENU_FIX_ZERO_DESC] = "Les nouveaux pools de mémoire GPU commencent remplis de zéros, comme la mémoire de la Switch.",
    [UI_MENU_BIND_SKIP] = "Sauter les liaisons répétées",
    [UI_MENU_BIND_SKIP_DESC] = "Optimisation : saute les liaisons qui n'ont pas changé.",
    [UI_MENU_SHADER_MEMO] = "Mémo des shaders", [UI_MENU_SHADER_MEMO_DESC] = "Optimisation : retient les recherches de shaders.",
    [UI_MENU_PIPE_MEMO] = "Mémo des pipelines", [UI_MENU_PIPE_MEMO_DESC] = "Optimisation : réutilise le pipeline du draw précédent quand son état se répète.",
    [UI_MENU_TEX_MEMO] = "Mémo des liaisons de textures", [UI_MENU_TEX_MEMO_DESC] = "Optimisation : retient les descripteurs de textures entre les draws.",
    [UI_MENU_DESC_TEMPLATE] = "Modèles de descripteurs",
    [UI_MENU_DESC_TEMPLATE_DESC] = "Optimisation : met à jour les descripteurs en bloc.",
    [UI_MENU_PUSH_SKIP] = "Sauter les push constants répétées",
    [UI_MENU_PUSH_SKIP_DESC] = "Optimisation : saute les push constants qui n'ont pas changé.",
    [UI_MENU_SPLIT_DRAWS] = "Envoi anticipé au GPU",
    [UI_MENU_SPLIT_DRAWS_DESC] = "Envoie le travail GPU par morceaux tous les N draws entre les passes pour que le GPU commence plus tôt ; 0 le désactive.",
    [UI_MENU_VK_VALIDATION] = "Couche de validation",
    [UI_MENU_VK_VALIDATION_DESC] = "Vérifie chaque appel Vulkan et ralentit beaucoup le jeu.",
    [UI_MENU_VK_VALIDATION_MISSING] = "La couche de validation n'est pas installée.",
    [UI_MENU_DLAA_MISSING] = "DLSS n'est pas disponible : il faut une carte NVIDIA RTX et nvngx_dlss.dll, que le build Windows télécharge. Relancez le build, ou mettez le fichier à côté de AgeOfCalamityRecomp.exe, et redémarrez. Ou réglez le moteur sur Automatique ou AMD FSR 3.1.",
    [UI_MENU_VK_DYN_VERTEX] = "État des sommets dynamique",
    [UI_MENU_VK_DYN_VERTEX_DESC] = "Force l'entrée de sommets dynamique, activée ou désactivée ; le défaut dépend du GPU.",
    [UI_MENU_VK_DYN_A2C] = "Alpha to coverage dynamique",
    [UI_MENU_VK_DYN_A2C_DESC] = "Force l'alpha to coverage dynamique, activé ou désactivé. Par défaut : désactivé avec le "
                                "pilote AMD pour RDNA 2 et plus ancien, qui dessine alors les personnages en noir, activé "
                                "ailleurs.",
    [UI_MENU_VK_SUBGROUP_PIN] = "Fixer la taille des subgroups",
    [UI_MENU_VK_SUBGROUP_PIN_DESC] = "Fixe la taille des subgroups des shaders.",
    [UI_MENU_TRACE] = "Journal de performances", [UI_MENU_TRACE_DESC] = "Écrit les temps d'image dans un fichier.",
    [UI_MENU_FLIGHT] = "Enregistreur de vol",
    [UI_MENU_FLIGHT_DESC] = "Garde un court historique et l'enregistre quand une image se fige.",
    [UI_MENU_FLIGHT_SPIKE_MS] = "Seuil de blocage (ms)",
    [UI_MENU_FLIGHT_SPIKE_MS_DESC] = "Temps d'image compté comme un blocage.",
    [UI_MENU_INPUT_RECORD] = "Enregistrement des entrées",
    [UI_MENU_INPUT_RECORD_DESC] = "Écrit vos entrées dans un fichier pour les rapports de bug.",
    [UI_MENU_OPEN_LOG] = "Ouvrir le dossier des journaux", [UI_MENU_OPEN_LOG_DESC] = "Affiche le dossier des fichiers journaux.",
    [UI_MENU_COPY_INFO] = "Copier les infos système",
    [UI_MENU_COPY_INFO_DESC] = "Copie le GPU, le pilote, le processeur, la RAM et les réglages modifiés dans le presse-papiers.",
    [UI_MENU_COPY_INFO_DONE] = "Copié", [UI_MENU_ACTION_FAILED] = "n'a pas marché",
    [UI_MENU_RESET] = "Remettre tous les réglages par défaut", [UI_MENU_RESET_DESC] = "Concerne tous les onglets.",
    [UI_MENU_RESET_CONFIRM] = "Remettre chaque réglage à sa valeur par défaut ?", [UI_MENU_RESET_YES] = "Réinitialiser",
    [UI_MENU_RESET_NO] = "Annuler",

    [UI_MENU_OV_FPS] = "%.0f FPS", [UI_MENU_OV_FRAME_MS] = "%.1f ms", [UI_MENU_OV_GAME_PCT] = "jeu %.0f%%",
    [UI_MENU_OV_MAX_MS] = "max %.1f ms",
    [UI_MENU_OV_GPU] = "GPU", [UI_MENU_OV_CPU] = "CPU", [UI_MENU_OV_RAM] = "RAM", [UI_MENU_OV_VRAM] = "VRAM",
    [UI_MENU_OV_NONE] = "-",
    [UI_MENU_OV_REPLAY] = "Préparation GPU", [UI_MENU_OV_GPU_TIME] = "GPU occupé par image",
    [UI_MENU_OV_PIPE_WAIT] = "Attentes de pipelines",
    [UI_MENU_OV_SHADER_WAIT] = "Attentes de shaders", [UI_MENU_OV_TEX_UPLOADS] = "Envois de textures",
    [UI_MENU_OV_FMT_PCT] = "%.0f%%", [UI_MENU_OV_FMT_TEMP] = "%.0f °C", [UI_MENU_OV_FMT_POWER] = "%.0f W",
    [UI_MENU_OV_FMT_MHZ] = "%.0f MHz", [UI_MENU_OV_FMT_GB] = "%.1f Go", [UI_MENU_OV_FMT_GB_OF] = "/ %.1f Go",
    [UI_MENU_OV_GAME_GB] = "jeu %.1f Go", [UI_MENU_OV_FMT_MS] = "%.2f ms", [UI_MENU_OV_PER_FRAME] = "%.1f par image",
    [UI_MENU_OV_WINDOW] = "Fenêtre", [UI_MENU_OV_OUTPUT] = "Image du jeu", [UI_MENU_OV_SCENE] = "Scène 3D",
    [UI_MENU_OV_SCALING] = "Mise à l'échelle", [UI_MENU_OV_LIMIT] = "Limite d'images", [UI_MENU_OV_PRESENT] = "Présentation",
    [UI_MENU_OV_FMT_SIZE] = "%ux%u", [UI_MENU_OV_FMT_SCENE] = "%ux%u, %.0f%%",
    [UI_MENU_OV_FMT_FSR] = "AMD FSR 1, netteté %d%%", [UI_MENU_OV_SCALE_NONE] = "aucune (1:1)",
    [UI_MENU_OV_SCALE_BILINEAR] = "bilinéaire",
    [UI_MENU_OV_SCALE_DOWN] = "bilinéaire (FSR ne fait qu'agrandir)",
    [UI_MENU_TAB_CHEATS] = "Triche",
    [UI_MENU_CHEATS_NONE] = "Aucun code de triche pour cette version du jeu. Les codes d'un mod sont un fichier de triche "
                            "Atmosphère dans son dossier cheats, nommé d'après le build du jeu : "
                            "mods\\<mod>\\cheats\\%s. Relancez le jeu pour les lire. Pour les codes qui modifient le code "
                            "du jeu, le jeu se recompile au lancement (quelques minutes).",
    [UI_MENU_CHEATS_MOD] = "%s : %d codes",
    [UI_MENU_CHEAT_CODE_DESC] = "Modifie le code du jeu. Bascule tout de suite.",
    [UI_MENU_CHEAT_KEY_DESC] = "Modifie le code du jeu tant que vous maintenez %s. Bascule tout de suite.",
    [UI_MENU_CHEAT_DATA_DESC] = "Écrit dans la mémoire du jeu à chaque image tant qu'il est activé.",
    [UI_MENU_CHEAT_MASTER] = "toujours actif",
    [UI_MENU_CHEAT_MASTER_DESC] = "Le code maître du fichier : il est toujours actif.",
    [UI_MENU_CHEAT_RESTORE] = "inutile",
    [UI_MENU_CHEAT_RESTORE_DESC] = "Il ne fait que réécrire le code d'origine du jeu. Désactiver un code le fait déjà.",
    [UI_MENU_CHEAT_REBUILD] = "après la recompilation",
    [UI_MENU_CHEAT_REBUILD_DESC] = "Ce code modifie le code du jeu et il est nouveau ou modifié depuis la compilation du "
                                   "jeu. Le jeu l'intègre à son prochain lancement (quelques minutes), ou utilisez "
                                   "Recompiler le jeu maintenant.",
    [UI_MENU_CHEAT_UNSUPPORTED] = "non pris en charge",
    [UI_MENU_CHEAT_UNSUPPORTED_DESC] = "Ce code ne peut pas être utilisé avec cette version du jeu :",
    [UI_MENU_CHEATS_REBUILD] = "Recompiler le jeu maintenant",
    [UI_MENU_CHEATS_REBUILD_DESC] = "Certains codes modifient le code du jeu et n'y sont pas intégrés : ils sont nouveaux "
                                    "ou modifiés, ou leur dernière recompilation a échoué ou a été annulée. Ferme le jeu ; "
                                    "le setup les intègre (quelques minutes) et relance le jeu. La progression depuis "
                                    "votre dernière sauvegarde est perdue.",
    [UI_MENU_MODS_EXEFS_UNSUPPORTED] = "Ce mod modifie le code du jeu (exefs) d'une façon que le portage ne peut pas "
                                       "appliquer, donc tout le mod reste désactivé. %s",
    [UI_MENU_MODS_EXEFS_OTHER] = "Ses modifications de code (exefs) sont faites pour une autre version du jeu.",
    [UI_MENU_MODS_EXEFS_ON] = "Modifications de code utilisées : %d fichiers.",
    [UI_MENU_MODS_REPLACES_BUILTIN] = "Modifications de code utilisées : %d fichiers. Elles remplacent le patch %s du "
                                      "portage, qui reste désactivé tant que ce mod est actif.",
    // FSR 3.1 and gyro calibration
    [UI_MENU_DLAA_ENGINE] = "Moteur",
    [UI_MENU_DLAA_ENGINE_DESC] = "Lequel est utilisé. Automatique : NVIDIA DLSS si la carte et nvngx_dlss.dll le "
                                "permettent, sinon AMD FSR 3.1. FSR 3.1 marche sur les cartes AMD, Intel et "
                                "NVIDIA. Prend effet après un redémarrage.",
    [UI_MENU_DLAA_ENGINE_AUTO] = "Automatique",
    [UI_MENU_DLAA_ENGINE_DLSS] = "NVIDIA DLSS",
    [UI_MENU_DLAA_ENGINE_FSR3] = "AMD FSR 3.1",
    [UI_MENU_FSR3_SHARPNESS] = "Netteté FSR 3.1",
    [UI_MENU_FSR3_SHARPNESS_DESC] = "L'accentuation propre à FSR 3.1 sur la scène 3D, contre le flou de son accumulation "
                                   "temporelle (FSR 3.1 seulement) ; 0 la désactive. S'applique tout de suite.",
    [UI_MENU_GYRO_CALIBRATION] = "Calibration du gyroscope",
    [UI_MENU_GYRO_CALIBRATION_DESC] = "Retire la dérive mesurée du gyroscope. Désactivé : le gyroscope brut.",
    [UI_MENU_CALIBRATE_GYRO] = "Calibrer le gyroscope",
    [UI_MENU_CALIBRATE_GYRO_DESC] = "Posez la manette sur une surface plane, appuyez, et ne la touchez plus pendant 2 "
                                    "secondes. La visée ne dérive plus sur le côté. Gardé pour cette manette.",
    [UI_MENU_CALIB_NONE] = "non calibré",
    [UI_MENU_CALIB_COLLECTING] = "ne bougez plus...",
    [UI_MENU_CALIB_DONE] = "calibré",
    [UI_MENU_CALIB_FAILED] = "pas de données du gyroscope",
    [UI_MENU_CALIB_MOVED] = "elle a bougé, recommencez",
};

#define SWKBD_ONLY(name, ...) static const char *const name[UI_TEXT_COUNT] = {SWKBD(__VA_ARGS__)}
SWKBD_ONLY(text_ja, "文字入力", "名前を入力してください", "ウマ", "OK", "キャンセル", "コントローラー: A = OK、B = キャンセル");
SWKBD_ONLY(text_de, "Texteingabe", "Gib einen Namen ein", "Pferd", "OK", "Abbrechen", "Controller: A = OK, B = Abbrechen");
SWKBD_ONLY(text_it, "Inserimento testo", "Inserisci un nome", "Cavallo", "OK", "Annulla", "Controller: A = OK, B = Annulla");
SWKBD_ONLY(text_es, "Introducir texto", "Introduce un nombre", "Caballo", "Aceptar", "Cancelar",
           "Mando: A = Aceptar, B = Cancelar");
SWKBD_ONLY(text_nl, "Tekstinvoer", "Voer een naam in", "Paard", "OK", "Annuleren", "Controller: A = OK, B = Annuleren");
SWKBD_ONLY(text_zh_hant, "文字輸入", "請輸入名字", "馬兒", "確定", "取消", "手把：A 確定，B 取消");
SWKBD_ONLY(text_ko, "문자 입력", "이름을 입력하세요", "말", "확인", "취소", "컨트롤러: A 확인, B 취소");
SWKBD_ONLY(text_ru, "Ввод текста", "Введите имя", "Лошадь", "ОК", "Отмена", "Контроллер: A — ОК, B — отмена");
SWKBD_ONLY(text_zh_hans, "文字输入", "请输入名字", "马儿", "确定", "取消", "手柄：A 确定，B 取消");

static const Row rows[] = {
    {"en", text_en}, {"ja", text_ja}, {"fr", text_fr}, {"de", text_de}, {"it", text_it}, {"es", text_es},
    {"nl", text_nl}, {"ru", text_ru}, {"zh-Hans", text_zh_hans}, {"zh-Hant", text_zh_hant},
    {"ko", text_ko},
};

const char *ui_text_lang(const char *code) {
    static const struct { const char *code, *lang; } map[] = {
        {"en-US", "en"}, {"en-GB", "en"}, {"fr-CA", "fr"}, {"es-419", "es"},
        {"zh-CN", "zh-Hans"}, {"zh-TW", "zh-Hant"},
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        if (!strcmp(code, map[i].code)) return map[i].lang;
    return code;
}

static const Row *find_row(const char *lang) {
    for (size_t i = 0; i < sizeof rows / sizeof *rows; i++)
        if (!strcmp(rows[i].lang, lang)) return &rows[i];
    return NULL;
}

int ui_text_defined(const char *lang, UiTextId id) {
    const Row *r = find_row(lang);
    return (unsigned)id < UI_TEXT_COUNT && r && r->text[id] && *r->text[id];
}

const char *ui_text_in(const char *lang, UiTextId id) {
    if ((unsigned)id >= UI_TEXT_COUNT) return "";
    return ui_text_defined(lang, id) ? find_row(lang)->text[id] : rows[0].text[id];
}

const char *ui_text(UiTextId id) { return ui_text_in(ui_text_lang(ov_language()), id); }

// A language's own strings, for the font that draws them: the merged system font must cover the whole menu of the
// language, not only its name (ui/menu.cpp fonts_up).
void ui_text_visit(const char *lang, void (*visit)(void *ctx, const char *text), void *ctx) {
    const Row *r = find_row(lang);
    if (!r) return;
    for (int id = 0; id < (int)UI_TEXT_COUNT; id++)
        if (r->text[id] && *r->text[id]) visit(ctx, r->text[id]);
}

// A language the menu is translated into: its row has the menu's own texts, not only the keyboard's.
static const Row *menu_row(const char *lang) {
    const Row *r = find_row(ui_text_lang(lang));
    return r && r->text[UI_MENU_CONTINUE] ? r : NULL;
}

const char *menu_lang(void) {
    const char *v = cfg_get("ACR_MENU_LANG");
    const Row *r = v && *v ? menu_row(v) : menu_row(ov_system_language());
    return r ? r->lang : "en";
}

const char *menu_text(UiTextId id) { return ui_text_in(menu_lang(), id); }

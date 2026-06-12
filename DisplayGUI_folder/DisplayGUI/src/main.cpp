//  When new UI is uploaded: insert line below again.
//  DisplayGUI\lib\ui\src\screens\ui_ScreenPresetsSelect.c
//  void ui_event_ButtonGoToBewerken(lv_event_t * e)
//  {
//    lv_event_code_t event_code = lv_event_get_code(e);
//
//    if(event_code == LV_EVENT_CLICKED) {
//        PreparePresetEditScreen(); // MANUAL ADDITION: must be re-added after every SquareLine export.
//        _ui_screen_change(&ui_ScreenPresetEdit, LV_SCR_LOAD_ANIM_FADE_ON, 500, 0, &ui_ScreenPresetEdit_screen_init);
//    }
//  }

// Arduino core for setup/loop, Serial, delays, and ESP32 runtime helpers.
#include <Arduino.h>
// ESP32 non-volatile storage used to persist preset data across power cycles.
#include <Preferences.h>
// LVGL graphics library used by the generated UI.
#include <lvgl.h>
// Mesh networking library used to communicate with the other ESP32 nodes.
#include <painlessMesh.h>
// Panel driver for the display hardware.
#include <ESP_Panel_Library.h>
// I/O expander driver for the panel control pins.
#include <ESP_IOExpander_Library.h>
// Generated SquareLine UI declarations.
#include <ui.h>

// I/O expander pin assignments used by the display hardware.
#define TP_RST 1
#define LCD_BL 2
#define LCD_RST 3
#define SD_CS 4
#define USB_SEL 5

// I2C bus number used by the expander.
#define I2C_MASTER_NUM 0

// Mesh network settings, kept aligned with the controller firmware.
#define MESH_PREFIX "PIMM"
#define MESH_PASSWORD "Odisee1234"
#define MESH_PORT 5555

// LVGL task and buffer tuning values.
#define LVGL_TASK_MAX_DELAY_MS (500)
#define LVGL_TASK_MIN_DELAY_MS (1)
#define LVGL_TASK_STACK_SIZE (4 * 1024)
#define LVGL_TASK_PRIORITY (2)
#define LVGL_BUF_SIZE (ESP_PANEL_LCD_H_RES * 20)

// Mesh task configuration values.
#define MESH_TASK_STACK_SIZE (6 * 1024)
#define MESH_TASK_PRIORITY (2)

// Display panel instance created during startup.
static ESP_Panel *panel = NULL;
// Recursive mutex used to protect LVGL calls.
static SemaphoreHandle_t lvgl_mux = NULL;

// Flag that tells LVGL to use a simulated touch instead of the hardware touch controller.
static volatile bool simulated_touch_active = false;
// X coordinate for the simulated touch point.
static volatile int simulated_touch_x = 0;
// Y coordinate for the simulated touch point.
static volatile int simulated_touch_y = 0;

// Current target height for MotorController1 stored in tenths of a millimeter.
static volatile int target_height_motor1_tenths_mm = 0;
// Current target height for MotorController2 stored in tenths of a millimeter.
static volatile int target_height_motor2_tenths_mm = 0;
// Actual height reported by MotorController1 stored in tenths of a millimeter.
static volatile int actual_height_motor1_tenths_mm = 0;
// Actual height reported by MotorController2 stored in tenths of a millimeter.
static volatile int actual_height_motor2_tenths_mm = 0;
// True once MotorController1 has sent at least one actual height in this power cycle.
// The HoogteModule1 label shows 'Niet gekend' until this flag is set.
static bool motor1_height_known = false;
// True once MotorController2 has sent at least one actual height in this power cycle.
// The HoogteModule2 label shows 'Niet gekend' until this flag is set.
static bool motor2_height_known = false;
// Flag that tells the GUI to keep both motor controllers synchronized.
static volatile bool sync_target_heights = false;
// Active motor controller selector, where false means MotorController1 and true means MotorController2.
static volatile bool target_motor_controller_two = false;
// Current encoder step size in tenths of a millimeter.
static volatile int encoder_step_size_tenths_mm = 1;
// True while the encoder button switches between modules; false while it changes the step size.
static bool encoder_knob_switches_module = true;

// Mesh node ID of MotorController1 (0 = not yet heard from).
static uint32_t motor1_node_id = 0;
// Mesh node ID of MotorController2 (0 = not yet heard from).
static uint32_t motor2_node_id = 0;
// True while MotorController1 is reachable on the mesh.
static volatile bool motor1_connected = false;
// True while MotorController2 is reachable on the mesh.
static volatile bool motor2_connected = false;

// Last alarm code received from each motor controller (0 = no alarm).
static volatile int motor1_alarm_code = 0;
static volatile int motor2_alarm_code = 0;

// Set when target heights change; cleared after a quiet period by the NVS save timer.
static volatile bool target_heights_dirty = false;
// millis() of the last target height change, used to debounce NVS writes.
static volatile uint32_t target_heights_last_changed_ms = 0;
// Save target heights to NVS this many ms after the last change.
#define TARGET_HEIGHT_SAVE_DEBOUNCE_MS 3000

// millis() timestamp of the last received message from each controller (0 = never seen).
static volatile uint32_t motor1_last_seen_ms = 0;
static volatile uint32_t motor2_last_seen_ms = 0;
// A controller that sends no message within this window is considered offline.
// Motor controllers broadcast every 100 ms, so 2 s = 20 missed messages.
#define MOTOR_TIMEOUT_MS 2000

// Maximum length for a stored preset name.
#define PRESET_NAME_MAX 64
// Maximum length for a stored preset height string (e.g. "M1: 86,0mm").
#define PRESET_HEIGHT_MAX 32
// Total number of user-configurable presets.
#define PRESET_COUNT 4

// One stored preset containing a name and two motor heights.
struct preset_data_t {
    char name[PRESET_NAME_MAX];
    char m1[PRESET_HEIGHT_MAX];
    char m2[PRESET_HEIGHT_MAX];
};

// Preset data for all four slots, initialised to the factory default.
static preset_data_t presets[PRESET_COUNT] = {
    {"PIMM Engineering", "M1: 0,0mm", "M2: 0,0mm"},
    {"PIMM Engineering", "M1: 0,0mm", "M2: 0,0mm"},
    {"PIMM Engineering", "M1: 0,0mm", "M2: 0,0mm"},
    {"PIMM Engineering", "M1: 0,0mm", "M2: 0,0mm"},
};

// Which preset is currently selected on ScreenPresetsSelect (0 = none, 1–4).
static int selected_preset = 0;

// Which preset is currently being edited on ScreenPresetEdit (1–4).
static int edit_preset = 1;

// Scheduler required by painlessMesh.
static Scheduler userScheduler;
// Mesh network object used for receive and broadcast operations.
static painlessMesh mesh;

// Broadcast the current target height to the selected motor controller.
static void send_target_height_message();

// Refresh the Sync button color so the active state is visible.
static void update_home_sync_button();

// Refresh the Step button label so it shows the current encoder step size.
static void update_home_step_button_label();

// Return the stored target height for the currently selected motor controller.
static int get_selected_target_height_tenths_mm();

// Clamp a target height so it stays within the allowed range.
static int clamp_target_height_tenths_mm(int value_tenths_mm);

// Format one target height value as "Doel: --,-mm" or "Doel: --,-mm MAX!".
static void format_target_height_text(int value_tenths_mm, char *buffer, size_t buffer_size);

// Refresh the Home screen target-height labels so they show the current values.
static void update_home_target_height_labels();

// Refresh the Home screen actual-height labels so they show the values reported by the controllers.
static void update_home_actual_height_labels();

// Refresh the Module1Connected and Module2Connected label colors to reflect current mesh state.
static void update_home_connection_labels();

// Refresh all alarm-related UI elements to reflect the current alarm state of both controllers.
static void update_alarm_display();
// Color the InfoModule1/2 borders Red when a module is unavailable, Green when available.
static void update_info_module_borders();

// Take the LVGL recursive mutex before touching LVGL objects.
static void lvgl_port_lock(int timeout_ms)
{
    const TickType_t timeout_ticks = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    xSemaphoreTakeRecursive(lvgl_mux, timeout_ticks);
}

// Release the LVGL recursive mutex after LVGL work is done.
static void lvgl_port_unlock(void)
{
    xSemaphoreGiveRecursive(lvgl_mux);
}

// Store the latest simulated touch state for LVGL to read later.
static void set_simulated_touch(int x, int y, bool active)
{
    simulated_touch_x = x; // Save the X coordinate from the incoming touch message.
    simulated_touch_y = y; // Save the Y coordinate from the incoming touch message.
    simulated_touch_active = active; // Record whether the touch should be reported as pressed.
}

// Return the mesh label for the currently selected motor controller.
static String get_selected_motor_controller_name()
{
    return target_motor_controller_two ? "MotorController2" : "MotorController1"; // Map the selector flag to a controller name.
}

// Return the outgoing message tag for the currently selected motor controller.
static String get_selected_target_height_tag()
{
    return target_motor_controller_two ? "TargetHeightMotor2" : "TargetHeightMotor1"; // Map the selector flag to the full message name.
}

// Return the outgoing message tag for MotorController1.
static String get_motor1_target_height_tag()
{
    return "TargetHeightMotor1"; // Use the exact controller tag expected by MotorController1.
}

// Return the outgoing message tag for MotorController2.
static String get_motor2_target_height_tag()
{
    return "TargetHeightMotor2"; // Use the exact controller tag expected by MotorController2.
}

// Return the current encoder step size in tenths of a millimeter.
static int get_encoder_step_size_tenths_mm()
{
    return encoder_step_size_tenths_mm; // Expose the active step size for the encoder handler.
}

// Clamp a target height so it stays between 0 and 860 tenths of a millimeter.
static int clamp_target_height_tenths_mm(int value_tenths_mm)
{
    if (value_tenths_mm < 0) { // Prevent the height from going below the minimum allowed value.
        return 0; // Return the lowest allowed height.
    }

    if (value_tenths_mm > 860) { // Prevent the height from going above the maximum allowed value.
        return 860; // Return the highest allowed height.
    }

    return value_tenths_mm; // Return values that are already in range.
}

// Format one target height value as "Doel: --,-mm" or "Doel: --,-mm MAX!".
static void format_target_height_text(int value_tenths_mm, char *buffer, size_t buffer_size)
{
    value_tenths_mm = clamp_target_height_tenths_mm(value_tenths_mm); // Keep the formatted value inside the allowed range.

    if (value_tenths_mm == 860) { // Use a special label when the target reaches the maximum allowed value.
        snprintf(buffer, buffer_size, "Doel: 86,0mm MAX!"); // Build the requested maximum-value label text.
        return; // Stop after formatting the maximum label.
    }

    const int absolute_value = value_tenths_mm < 0 ? -value_tenths_mm : value_tenths_mm; // Work with a positive value for formatting.
    const int whole_mm = absolute_value / 10; // Extract the millimeter part.
    const int tenths_mm = absolute_value % 10; // Extract the decimal part in tenths of a millimeter.
    const char *sign = value_tenths_mm < 0 ? "-" : ""; // Keep the minus sign only for negative values.
    snprintf(buffer, buffer_size, "Doel: %s%d,%dmm", sign, whole_mm, tenths_mm); // Build the label text in the requested format.
}

// Refresh the Home screen target-height labels so they show the current values.
static void update_home_target_height_labels()
{
    char label_text[24]; // Keep a small reusable buffer for one label line.

    if (ui_DoelModule1 != NULL) { // Skip the label if the screen has not been created yet.
        format_target_height_text(target_height_motor1_tenths_mm, label_text, sizeof(label_text)); // Format the MotorController1 height text.
        lv_label_set_text(ui_DoelModule1, label_text); // Show the formatted MotorController1 target height.
    }

    if (ui_DoelModule2 != NULL) { // Skip the label if the screen has not been created yet.
        format_target_height_text(target_height_motor2_tenths_mm, label_text, sizeof(label_text)); // Format the MotorController2 height text.
        lv_label_set_text(ui_DoelModule2, label_text); // Show the formatted MotorController2 target height.
    }
}

// Format an actual height value as "--,-mm" (e.g. "12,5mm").
static void format_actual_height_text(int value_tenths_mm, char *buffer, size_t buffer_size)
{
    const int whole_mm = value_tenths_mm / 10; // Extract the whole-millimeter part.
    const int tenths   = value_tenths_mm % 10; // Extract the tenths-of-millimeter part.
    snprintf(buffer, buffer_size, "%d,%dmm", whole_mm, tenths); // Build the label text in the requested format.
}

// Refresh the Home screen actual-height labels so they show the values reported by the controllers.
// Labels are left unchanged (showing 'Niet gekend') until the module has sent at least one height.
static void update_home_actual_height_labels()
{
    char label_text[12]; // Small reusable buffer for one label line.

    if (ui_HoogteModule1 != NULL && motor1_height_known) {
        format_actual_height_text(actual_height_motor1_tenths_mm, label_text, sizeof(label_text));
        lv_label_set_text(ui_HoogteModule1, label_text);
    }

    if (ui_HoogteModule2 != NULL && motor2_height_known) {
        format_actual_height_text(actual_height_motor2_tenths_mm, label_text, sizeof(label_text));
        lv_label_set_text(ui_HoogteModule2, label_text); // Show the formatted actual height on the Home screen.
    }
}

// Record that target heights changed and reset the debounce timer.
static void mark_target_heights_dirty(uint32_t now_ms)
{
    target_heights_dirty = true;
    target_heights_last_changed_ms = now_ms;
}

// Persist both target heights to NVS so they survive a power cycle.
static void save_target_heights_to_nvs()
{
    Preferences prefs;
    prefs.begin("heights", false);
    prefs.putInt("m1", target_height_motor1_tenths_mm);
    prefs.putInt("m2", target_height_motor2_tenths_mm);
    prefs.end();
}

// Persist the encoder step size and knob mode to NVS.
static void save_settings_to_nvs()
{
    Preferences prefs;
    prefs.begin("settings", false);
    prefs.putInt("step", encoder_step_size_tenths_mm);
    prefs.putBool("enc_mod", encoder_knob_switches_module);
    prefs.end();
}

// Restore the encoder step size and knob mode from NVS, using sensible defaults if not yet saved.
static void load_settings_from_nvs()
{
    Preferences prefs;
    prefs.begin("settings", true);
    encoder_step_size_tenths_mm  = prefs.getInt("step",    1);
    encoder_knob_switches_module = prefs.getBool("enc_mod", true);
    prefs.end();
}

// Load target heights from NVS, falling back to 0 mm if no value was previously saved.
static void load_target_heights_from_nvs()
{
    Preferences prefs;
    prefs.begin("heights", true);
    target_height_motor1_tenths_mm = clamp_target_height_tenths_mm(prefs.getInt("m1", 0));
    target_height_motor2_tenths_mm = clamp_target_height_tenths_mm(prefs.getInt("m2", 0));
    prefs.end();
}

// Color InfoModule1/2 borders: Red when the module is disconnected or has an active alarm, Green when available.
static void update_info_module_borders()
{
    const bool m1_unavailable = (!motor1_connected || motor1_alarm_code > 0);
    const bool m2_unavailable = (!motor2_connected || motor2_alarm_code > 0);

    if (ui_InfoModule1 != NULL) {
        ui_object_set_themeable_style_property(ui_InfoModule1, LV_PART_MAIN | LV_STATE_DEFAULT, LV_STYLE_BORDER_COLOR,
            m1_unavailable ? _ui_theme_color_Red : _ui_theme_color_Green);
        ui_object_set_themeable_style_property(ui_InfoModule1, LV_PART_MAIN | LV_STATE_DEFAULT, LV_STYLE_BORDER_OPA,
            m1_unavailable ? _ui_theme_alpha_Red : _ui_theme_alpha_Green);
    }
    if (ui_InfoModule2 != NULL) {
        ui_object_set_themeable_style_property(ui_InfoModule2, LV_PART_MAIN | LV_STATE_DEFAULT, LV_STYLE_BORDER_COLOR,
            m2_unavailable ? _ui_theme_color_Red : _ui_theme_color_Green);
        ui_object_set_themeable_style_property(ui_InfoModule2, LV_PART_MAIN | LV_STATE_DEFAULT, LV_STYLE_BORDER_OPA,
            m2_unavailable ? _ui_theme_alpha_Red : _ui_theme_alpha_Green);
    }
}

// Apply the Module1Connected and Module2Connected label text colors — Green when connected, Red when not.
static void update_home_connection_labels()
{
    if (ui_Module1Connected != NULL) {
        ui_object_set_themeable_style_property(ui_Module1Connected, LV_PART_MAIN | LV_STATE_DEFAULT, LV_STYLE_TEXT_COLOR,
            motor1_connected ? _ui_theme_color_Green : _ui_theme_color_Red);
        ui_object_set_themeable_style_property(ui_Module1Connected, LV_PART_MAIN | LV_STATE_DEFAULT, LV_STYLE_TEXT_OPA,
            motor1_connected ? _ui_theme_alpha_Green : _ui_theme_alpha_Red);
    }

    if (ui_Module2Connected != NULL) {
        ui_object_set_themeable_style_property(ui_Module2Connected, LV_PART_MAIN | LV_STATE_DEFAULT, LV_STYLE_TEXT_COLOR,
            motor2_connected ? _ui_theme_color_Green : _ui_theme_color_Red);
        ui_object_set_themeable_style_property(ui_Module2Connected, LV_PART_MAIN | LV_STATE_DEFAULT, LV_STYLE_TEXT_OPA,
            motor2_connected ? _ui_theme_alpha_Green : _ui_theme_alpha_Red);
    }
    update_info_module_borders();
    update_home_sync_button();
}

// Refresh all alarm-related UI: Foutcodes label, reset button colors, and ScreenHome alarm labels.
static void update_alarm_display()
{
    const bool m1 = (motor1_alarm_code > 0);
    const bool m2 = (motor2_alarm_code > 0);

    const char *text;
    if      (!m1 && !m2) text = "Geen";
    else if ( m1 && !m2) text = "Motor 1";
    else if (!m1 &&  m2) text = "Motor 2";
    else                 text = "Motor 1 en Motor 2";

    const bool any_alarm = (m1 || m2);

    // Color both reset buttons Red when any alarm is active, LightBlue when clear.
    if (ui_ButtonResetAlarm != NULL) {
        ui_object_set_themeable_style_property(ui_ButtonResetAlarm, LV_PART_MAIN | LV_STATE_DEFAULT, LV_STYLE_BG_COLOR,
            any_alarm ? _ui_theme_color_Red : _ui_theme_color_LightBlue);
        ui_object_set_themeable_style_property(ui_ButtonResetAlarm, LV_PART_MAIN | LV_STATE_DEFAULT, LV_STYLE_BG_OPA,
            any_alarm ? _ui_theme_alpha_Red : _ui_theme_alpha_LightBlue);
    }
    if (ui_ButtonResetAlarm2 != NULL) {
        ui_object_set_themeable_style_property(ui_ButtonResetAlarm2, LV_PART_MAIN | LV_STATE_DEFAULT, LV_STYLE_BG_COLOR,
            any_alarm ? _ui_theme_color_Red : _ui_theme_color_LightBlue);
        ui_object_set_themeable_style_property(ui_ButtonResetAlarm2, LV_PART_MAIN | LV_STATE_DEFAULT, LV_STYLE_BG_OPA,
            any_alarm ? _ui_theme_alpha_Red : _ui_theme_alpha_LightBlue);
    }

    // Update the fault description label.
    if (ui_Foutcodes != NULL) {
        lv_label_set_text(ui_Foutcodes, text);
    }

    // Show or hide the ScreenHome alarm indicator per module.
    if (ui_AlarmModule1 != NULL) {
        if (m1) lv_obj_clear_flag(ui_AlarmModule1, LV_OBJ_FLAG_HIDDEN);
        else    lv_obj_add_flag(ui_AlarmModule1,   LV_OBJ_FLAG_HIDDEN);
    }
    if (ui_AlarmModule2 != NULL) {
        if (m2) lv_obj_clear_flag(ui_AlarmModule2, LV_OBJ_FLAG_HIDDEN);
        else    lv_obj_add_flag(ui_AlarmModule2,   LV_OBJ_FLAG_HIDDEN);
    }
    update_info_module_borders();
    update_home_sync_button();
}

// Apply the Home screen button colors so the selected controller is shown in green.
static void update_home_motor_selection_buttons()
{
    if (ui_ButtonSelectModule1 != NULL) { // Skip the button if the screen has not been created yet.
        if (target_motor_controller_two) { // Show MotorController2 as selected when the selector is on module 2.
            ui_object_set_themeable_style_property(ui_ButtonSelectModule1, LV_PART_MAIN | LV_STATE_DEFAULT, LV_STYLE_BG_COLOR, _ui_theme_color_LightBlue); // Restore the default color for Module 1.
            ui_object_set_themeable_style_property(ui_ButtonSelectModule1, LV_PART_MAIN | LV_STATE_DEFAULT, LV_STYLE_BG_OPA, _ui_theme_alpha_LightBlue); // Restore the default opacity for Module 1.
        } else { // Show MotorController1 as selected when the selector is on module 1.
            ui_object_set_themeable_style_property(ui_ButtonSelectModule1, LV_PART_MAIN | LV_STATE_DEFAULT, LV_STYLE_BG_COLOR, _ui_theme_color_Green); // Highlight Module 1 in green.
            ui_object_set_themeable_style_property(ui_ButtonSelectModule1, LV_PART_MAIN | LV_STATE_DEFAULT, LV_STYLE_BG_OPA, _ui_theme_alpha_Green); // Keep the green highlight fully visible.
        }
    }

    if (ui_ButtonSelectModule2 != NULL) { // Skip the button if the screen has not been created yet.
        if (target_motor_controller_two) { // Show MotorController2 as selected when the selector is on module 2.
            ui_object_set_themeable_style_property(ui_ButtonSelectModule2, LV_PART_MAIN | LV_STATE_DEFAULT, LV_STYLE_BG_COLOR, _ui_theme_color_Green); // Highlight Module 2 in green.
            ui_object_set_themeable_style_property(ui_ButtonSelectModule2, LV_PART_MAIN | LV_STATE_DEFAULT, LV_STYLE_BG_OPA, _ui_theme_alpha_Green); // Keep the green highlight fully visible.
        } else { // Restore the default look when Module 2 is not selected.
            ui_object_set_themeable_style_property(ui_ButtonSelectModule2, LV_PART_MAIN | LV_STATE_DEFAULT, LV_STYLE_BG_COLOR, _ui_theme_color_LightBlue); // Restore the default color for Module 2.
            ui_object_set_themeable_style_property(ui_ButtonSelectModule2, LV_PART_MAIN | LV_STATE_DEFAULT, LV_STYLE_BG_OPA, _ui_theme_alpha_LightBlue); // Restore the default opacity for Module 2.
        }
    }
}

// Apply the Sync button state to reflect availability: both controllers must be connected and alarm-free.
static void update_home_sync_button()
{
    if (ui_ButtonSelectSync == NULL) { // Skip the button if the screen has not been created yet.
        return;
    }

    const bool sync_available = (motor1_connected && motor2_connected
                                 && motor1_alarm_code == 0 && motor2_alarm_code == 0);

    // If sync is active but conditions are no longer met, deactivate it immediately.
    if (!sync_available && sync_target_heights) {
        sync_target_heights = false;
    }

    // Allow or block clicks based on availability.
    if (sync_available) {
        lv_obj_add_flag(ui_ButtonSelectSync, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_clear_flag(ui_ButtonSelectSync, LV_OBJ_FLAG_CLICKABLE);
    }

    if (ui_TextButtonSelectSync != NULL) {
        ui_object_set_themeable_style_property(ui_TextButtonSelectSync, LV_PART_MAIN | LV_STATE_DEFAULT, LV_STYLE_TEXT_COLOR,
            sync_available ? _ui_theme_color_DarkBlue : _ui_theme_color_Grey);
        ui_object_set_themeable_style_property(ui_TextButtonSelectSync, LV_PART_MAIN | LV_STATE_DEFAULT, LV_STYLE_TEXT_OPA,
            sync_available ? _ui_theme_alpha_DarkBlue : _ui_theme_alpha_Grey);
    }

    if (sync_target_heights) {
        ui_object_set_themeable_style_property(ui_ButtonSelectSync, LV_PART_MAIN | LV_STATE_DEFAULT, LV_STYLE_BG_COLOR, _ui_theme_color_Green);
        ui_object_set_themeable_style_property(ui_ButtonSelectSync, LV_PART_MAIN | LV_STATE_DEFAULT, LV_STYLE_BG_OPA, _ui_theme_alpha_Green);
        return;
    }

    ui_object_set_themeable_style_property(ui_ButtonSelectSync, LV_PART_MAIN | LV_STATE_DEFAULT, LV_STYLE_BG_COLOR, _ui_theme_color_LightBlue);
    ui_object_set_themeable_style_property(ui_ButtonSelectSync, LV_PART_MAIN | LV_STATE_DEFAULT, LV_STYLE_BG_OPA, _ui_theme_alpha_LightBlue);
}

// Refresh the Step button label so it shows the current encoder step size.
static void update_home_step_button_label()
{
    if (ui_TextButtonStap == NULL) { // Skip the label if the screen has not been created yet.
        return; // Stop when the Step label does not exist yet.
    }

    if (get_encoder_step_size_tenths_mm() == 10) { // Show 1.0 mm mode when the encoder step is ten tenths of a millimeter.
        lv_label_set_text(ui_TextButtonStap, "Stap: 1,0mm"); // Display the larger step size in the requested format.
        return; // Stop after updating the label for 1.0 mm mode.
    }

    lv_label_set_text(ui_TextButtonStap, "Stap: 0,1mm"); // Display the default 0.1 mm step size.
}

// Mirror the selected controller's current target height to the inactive controller.
static void mirror_selected_target_height_to_inactive_controller()
{
    const int selected_target_height_tenths_mm = clamp_target_height_tenths_mm(get_selected_target_height_tenths_mm()); // Read and clamp the active controller's stored target height.

    if (target_motor_controller_two) { // Copy MotorController2 to MotorController1 when module 2 is selected.
        target_height_motor1_tenths_mm = selected_target_height_tenths_mm; // Keep MotorController1 aligned with MotorController2.
        return; // Stop after copying the inactive controller.
    }

    target_height_motor2_tenths_mm = selected_target_height_tenths_mm; // Keep MotorController2 aligned with MotorController1.
}

// Write all four presets to NVS so they survive a power cycle.
static void save_presets_to_nvs()
{
    Preferences prefs;
    prefs.begin("presets", false);
    char key[8];
    for (int i = 0; i < PRESET_COUNT; i++) {
        snprintf(key, sizeof(key), "p%dname", i + 1);
        prefs.putString(key, presets[i].name);
        snprintf(key, sizeof(key), "p%dm1", i + 1);
        prefs.putString(key, presets[i].m1);
        snprintf(key, sizeof(key), "p%dm2", i + 1);
        prefs.putString(key, presets[i].m2);
    }
    prefs.end();
}

// Read all four presets from NVS, falling back to factory defaults if a key is missing.
static void load_presets_from_nvs()
{
    Preferences prefs;
    prefs.begin("presets", true);
    char key[8];
    for (int i = 0; i < PRESET_COUNT; i++) {
        snprintf(key, sizeof(key), "p%dname", i + 1);
        String name = prefs.getString(key, "PIMM Engineering");
        strncpy(presets[i].name, name.c_str(), PRESET_NAME_MAX - 1);
        presets[i].name[PRESET_NAME_MAX - 1] = '\0';

        snprintf(key, sizeof(key), "p%dm1", i + 1);
        String m1 = prefs.getString(key, "M1: 0,0mm");
        strncpy(presets[i].m1, m1.c_str(), PRESET_HEIGHT_MAX - 1);
        presets[i].m1[PRESET_HEIGHT_MAX - 1] = '\0';

        snprintf(key, sizeof(key), "p%dm2", i + 1);
        String m2 = prefs.getString(key, "M2: 0,0mm");
        strncpy(presets[i].m2, m2.c_str(), PRESET_HEIGHT_MAX - 1);
        presets[i].m2[PRESET_HEIGHT_MAX - 1] = '\0';
    }
    prefs.end();
}

// Strip the "M1: " / "M2: " prefix and "mm" suffix to get the raw numeric string for a text area.
static void extract_textarea_value(const char *stored, char *buffer, size_t size)
{
    const char *p = strchr(stored, ':');
    if (p == NULL) {
        strncpy(buffer, stored, size - 1);
        buffer[size - 1] = '\0';
        return;
    }
    p += 2; // skip ": "
    strncpy(buffer, p, size - 1);
    buffer[size - 1] = '\0';
    size_t len = strlen(buffer);
    if (len >= 2 && buffer[len - 2] == 'm' && buffer[len - 1] == 'm') {
        buffer[len - 2] = '\0';
    }
}

// Parse a stored height string like "M1: 12,5mm" to tenths of a millimeter.
static int parse_preset_height_tenths_mm(const char *stored)
{
    const char *p = strchr(stored, ':');
    if (p == NULL) return 0;
    p += 2;
    int whole = 0;
    while (*p >= '0' && *p <= '9') {
        whole = whole * 10 + (*p++ - '0');
    }
    int tenths = 0;
    if (*p == ',' || *p == '.') {
        p++;
        if (*p >= '0' && *p <= '9') {
            tenths = *p - '0';
        }
    }
    return clamp_target_height_tenths_mm(whole * 10 + tenths);
}

// Parse a raw textarea value (e.g. "12,56"), clamp to 86,0mm, and reformat as "whole,tenths".
// Accepts comma or period as decimal separator. Rounds to one decimal place: second decimal digit
// >= 5 rounds up, < 5 rounds down. Carry is propagated (e.g. 12,96 -> 13,0).
static void normalize_preset_height(const char *input, char *output, size_t output_size)
{
    const char *p = input;
    int whole = 0;
    while (*p >= '0' && *p <= '9') {
        whole = whole * 10 + (*p++ - '0');
    }
    int tenths = 0;
    if (*p == ',' || *p == '.') {
        p++;
        if (*p >= '0' && *p <= '9') {
            tenths = *p - '0';
            p++;
            if (*p >= '5' && *p <= '9') { // Round up when the next digit is 5 or more.
                tenths++;
            }
        }
    }
    if (tenths >= 10) { // Propagate carry from rounding (e.g. 12,96 -> 13,0).
        whole++;
        tenths = 0;
    }
    const int clamped = clamp_target_height_tenths_mm(whole * 10 + tenths);
    snprintf(output, output_size, "%d,%d", clamped / 10, clamped % 10);
}

// Write the stored preset name and height strings to the four preset rows on ScreenPresetsSelect.
static void update_all_preset_list_labels()
{
    lv_obj_t *name_labels[PRESET_COUNT] = {
        ui_NamePreset1, ui_NamePreset2, ui_NamePreset3, ui_NamePreset4,
    };
    lv_obj_t *m1_labels[PRESET_COUNT] = {
        ui_Preset1HoogteM1, ui_Preset2HoogteM1, ui_Preset3HoogteM1, ui_Preset4HoogteM1,
    };
    lv_obj_t *m2_labels[PRESET_COUNT] = {
        ui_Preset1HoogteM2, ui_Preset2HoogteM2, ui_Preset3HoogteM2, ui_Preset4HoogteM2,
    };

    for (int i = 0; i < PRESET_COUNT; i++) {
        if (name_labels[i] != NULL) lv_label_set_text(name_labels[i], presets[i].name);
        if (m1_labels[i] != NULL)   lv_label_set_text(m1_labels[i],   presets[i].m1);
        if (m2_labels[i] != NULL)   lv_label_set_text(m2_labels[i],   presets[i].m2);
    }
}

// Refresh the four select-button colors and the Toepassen / Bewerken button states.
static void update_preset_select_buttons()
{
    lv_obj_t *select_buttons[PRESET_COUNT] = {
        ui_ButtonSelectPreset1,
        ui_ButtonSelectPreset2,
        ui_ButtonSelectPreset3,
        ui_ButtonSelectPreset4,
    };

    for (int i = 0; i < PRESET_COUNT; i++) {
        if (select_buttons[i] == NULL) continue;
        const bool is_selected = (selected_preset == i + 1);
        ui_object_set_themeable_style_property(select_buttons[i], LV_PART_MAIN | LV_STATE_DEFAULT, LV_STYLE_BG_COLOR,
            is_selected ? _ui_theme_color_Green : _ui_theme_color_LightBlue);
        ui_object_set_themeable_style_property(select_buttons[i], LV_PART_MAIN | LV_STATE_DEFAULT, LV_STYLE_BG_OPA,
            is_selected ? _ui_theme_alpha_Green : _ui_theme_alpha_LightBlue);
    }

    const bool has_selection = (selected_preset != 0);

    if (ui_ButtonToepassen != NULL) {
        if (has_selection) {
            lv_obj_add_flag(ui_ButtonToepassen, LV_OBJ_FLAG_CLICKABLE);
        } else {
            lv_obj_clear_flag(ui_ButtonToepassen, LV_OBJ_FLAG_CLICKABLE);
        }
    }
    if (ui_TextButtonToepassen != NULL) {
        ui_object_set_themeable_style_property(ui_TextButtonToepassen, LV_PART_MAIN | LV_STATE_DEFAULT, LV_STYLE_TEXT_COLOR,
            has_selection ? _ui_theme_color_DarkBlue : _ui_theme_color_Grey);
        ui_object_set_themeable_style_property(ui_TextButtonToepassen, LV_PART_MAIN | LV_STATE_DEFAULT, LV_STYLE_TEXT_OPA,
            has_selection ? _ui_theme_alpha_DarkBlue : _ui_theme_alpha_Grey);
    }

    if (ui_ButtonGoToBewerken != NULL) {
        if (has_selection) {
            lv_obj_add_flag(ui_ButtonGoToBewerken, LV_OBJ_FLAG_CLICKABLE);
        } else {
            lv_obj_clear_flag(ui_ButtonGoToBewerken, LV_OBJ_FLAG_CLICKABLE);
        }
    }
    if (ui_TextButtonGoToBewerken != NULL) {
        ui_object_set_themeable_style_property(ui_TextButtonGoToBewerken, LV_PART_MAIN | LV_STATE_DEFAULT, LV_STYLE_TEXT_COLOR,
            has_selection ? _ui_theme_color_DarkBlue : _ui_theme_color_Grey);
        ui_object_set_themeable_style_property(ui_TextButtonGoToBewerken, LV_PART_MAIN | LV_STATE_DEFAULT, LV_STYLE_TEXT_OPA,
            has_selection ? _ui_theme_alpha_DarkBlue : _ui_theme_alpha_Grey);
    }
}

// Refresh the header labels and text areas on ScreenPresetEdit for the current edit_preset.
static void update_preset_edit_labels()
{
    const int idx = edit_preset - 1;
    char num_str[4];
    snprintf(num_str, sizeof(num_str), "%d", edit_preset);

    if (ui_EditPreset != NULL)         lv_label_set_text(ui_EditPreset, num_str);
    if (ui_EditNamePreset != NULL)     lv_label_set_text(ui_EditNamePreset, presets[idx].name);
    if (ui_EditPresetHoogteM1 != NULL) lv_label_set_text(ui_EditPresetHoogteM1, presets[idx].m1);
    if (ui_EditPresetHoogteM2 != NULL) lv_label_set_text(ui_EditPresetHoogteM2, presets[idx].m2);

    char value_buf[PRESET_HEIGHT_MAX];

    if (ui_InsertNameTextArea != NULL) {
        lv_textarea_set_text(ui_InsertNameTextArea, presets[idx].name);
    }
    if (ui_InsertM1TextArea != NULL) {
        extract_textarea_value(presets[idx].m1, value_buf, sizeof(value_buf));
        lv_textarea_set_text(ui_InsertM1TextArea, value_buf);
    }
    if (ui_InsertM2TextArea != NULL) {
        extract_textarea_value(presets[idx].m2, value_buf, sizeof(value_buf));
        lv_textarea_set_text(ui_InsertM2TextArea, value_buf);
    }
}

// Update the selected motor controller, refresh the Home screen buttons, and resend the active target height.
extern "C" void SetSelectedMotorController(int motor2_selected)
{
    target_motor_controller_two = motor2_selected != 0; // Store whether MotorController2 is currently selected.
    if (sync_target_heights) { // Keep both target heights equal while Sync is active.
        mirror_selected_target_height_to_inactive_controller(); // Copy the active controller height to the inactive one.
    }
    update_home_motor_selection_buttons(); // Refresh the two Home screen buttons so the active one turns green.
    update_home_target_height_labels(); // Refresh both Home-screen target labels so they show the latest values.
    update_home_step_button_label(); // Keep the Step label visible whenever the selected controller changes.
    send_target_height_message(); // Resend the current target height to the newly selected controller.
}

// Send a calibration command to the currently selected motor controller.
// Called by the Kalibreer event stub in ui_events.c, following the same pattern as the other button helpers.
extern "C" void SendCalibrateCommand(void)
{
    if (target_motor_controller_two) { // Send the calibration command to whichever controller is currently selected.
        mesh.sendBroadcast("CalibrateMotor2"); // Command MotorController2 to move up and find its limit switch.
    } else {
        mesh.sendBroadcast("CalibrateMotor1"); // Command MotorController1 to move up and find its limit switch.
    }
}

// Send calibration commands to both motor controllers simultaneously.
extern "C" void CalibrateAllMotors(void)
{
    mesh.sendBroadcast("CalibrateMotor1");
    mesh.sendBroadcast("CalibrateMotor2");
}

// Set ButtonToonLink Green and ButtonVerbergLink LightBlue.
extern "C" void HandleToonLink(void)
{
    if (ui_ButtonToonLink != NULL) {
        ui_object_set_themeable_style_property(ui_ButtonToonLink, LV_PART_MAIN | LV_STATE_DEFAULT, LV_STYLE_BG_COLOR, _ui_theme_color_Green);
        ui_object_set_themeable_style_property(ui_ButtonToonLink, LV_PART_MAIN | LV_STATE_DEFAULT, LV_STYLE_BG_OPA, _ui_theme_alpha_Green);
    }
    if (ui_ButtonVerbergLink != NULL) {
        ui_object_set_themeable_style_property(ui_ButtonVerbergLink, LV_PART_MAIN | LV_STATE_DEFAULT, LV_STYLE_BG_COLOR, _ui_theme_color_LightBlue);
        ui_object_set_themeable_style_property(ui_ButtonVerbergLink, LV_PART_MAIN | LV_STATE_DEFAULT, LV_STYLE_BG_OPA, _ui_theme_alpha_LightBlue);
    }
}

// Set ButtonVerbergLink Green and ButtonToonLink LightBlue.
extern "C" void HandleVerbergLink(void)
{
    if (ui_ButtonVerbergLink != NULL) {
        ui_object_set_themeable_style_property(ui_ButtonVerbergLink, LV_PART_MAIN | LV_STATE_DEFAULT, LV_STYLE_BG_COLOR, _ui_theme_color_Green);
        ui_object_set_themeable_style_property(ui_ButtonVerbergLink, LV_PART_MAIN | LV_STATE_DEFAULT, LV_STYLE_BG_OPA, _ui_theme_alpha_Green);
    }
    if (ui_ButtonToonLink != NULL) {
        ui_object_set_themeable_style_property(ui_ButtonToonLink, LV_PART_MAIN | LV_STATE_DEFAULT, LV_STYLE_BG_COLOR, _ui_theme_color_LightBlue);
        ui_object_set_themeable_style_property(ui_ButtonToonLink, LV_PART_MAIN | LV_STATE_DEFAULT, LV_STYLE_BG_OPA, _ui_theme_alpha_LightBlue);
    }
}

// Apply button colours that reflect the current encoder_knob_switches_module state.
// WisselenVanModule = Green when active, AanpassenVanDeStap = Green when active.
static void update_encoder_mode_buttons()
{
    if (ui_ButtonWisselenVanModule != NULL) {
        ui_object_set_themeable_style_property(ui_ButtonWisselenVanModule, LV_PART_MAIN | LV_STATE_DEFAULT, LV_STYLE_BG_COLOR,
            encoder_knob_switches_module ? _ui_theme_color_Green : _ui_theme_color_LightBlue);
        ui_object_set_themeable_style_property(ui_ButtonWisselenVanModule, LV_PART_MAIN | LV_STATE_DEFAULT, LV_STYLE_BG_OPA,
            encoder_knob_switches_module ? _ui_theme_alpha_Green : _ui_theme_alpha_LightBlue);
    }
    if (ui_ButtonAanpassenVanDeStap != NULL) {
        ui_object_set_themeable_style_property(ui_ButtonAanpassenVanDeStap, LV_PART_MAIN | LV_STATE_DEFAULT, LV_STYLE_BG_COLOR,
            encoder_knob_switches_module ? _ui_theme_color_LightBlue : _ui_theme_color_Green);
        ui_object_set_themeable_style_property(ui_ButtonAanpassenVanDeStap, LV_PART_MAIN | LV_STATE_DEFAULT, LV_STYLE_BG_OPA,
            encoder_knob_switches_module ? _ui_theme_alpha_LightBlue : _ui_theme_alpha_Green);
    }
}

// Activate WisselenVanModule mode: encoder button will switch between motor controllers.
extern "C" void HandleWisselenVanModule(void)
{
    encoder_knob_switches_module = true;
    update_encoder_mode_buttons();
    save_settings_to_nvs();
}

// Activate AanpassenVanDeStap mode: encoder button will toggle the step size.
extern "C" void HandleAanpassenVanDeStap(void)
{
    encoder_knob_switches_module = false;
    update_encoder_mode_buttons();
    save_settings_to_nvs();
}

// Toggle between the two encoder-knob modes and refresh the indicator buttons.
extern "C" void HandleEncoderKnopSwitch(void)
{
    encoder_knob_switches_module = !encoder_knob_switches_module;
    update_encoder_mode_buttons();
    save_settings_to_nvs();
}

// Acknowledge and clear the alarm state for both controllers.
extern "C" void HandleAlarmReset(void)
{
    motor1_alarm_code = 0;
    motor2_alarm_code = 0;
    mesh.sendBroadcast("ResetAlarmMotor1");
    mesh.sendBroadcast("ResetAlarmMotor2");
    lvgl_port_lock(-1);
    update_alarm_display();
    lvgl_port_unlock();
}

// Toggle the encoder step size between 0.1 mm and 1.0 mm.
extern "C" void ToggleStepSizeMode(void)
{
    if (encoder_step_size_tenths_mm == 1) { // Switch from the small step to the large step.
        encoder_step_size_tenths_mm = 10; // Use 1.0 mm encoder steps.
    } else { // Switch back to the small step.
        encoder_step_size_tenths_mm = 1; // Use 0.1 mm encoder steps.
    }

    update_home_step_button_label(); // Refresh the Step button so the current mode is visible.
    save_settings_to_nvs();
}

// Toggle synchronized control for both motor controllers.
extern "C" void ToggleSyncMode(void)
{
    sync_target_heights = !sync_target_heights; // Flip the synchronized-control state.

    if (sync_target_heights) { // When Sync turns on, force both controllers to use the active height.
        mirror_selected_target_height_to_inactive_controller(); // Copy the active controller height to the inactive one.
    }

    update_home_sync_button(); // Refresh the Sync button so the active state turns green.
    update_home_target_height_labels(); // Refresh both target labels because Sync may have copied values.
    send_target_height_message(); // Send the current height state to the motor controller(s).
}

// Populate ScreenPresetsSelect labels from stored data and apply the correct button states.
extern "C" void InitPresetsSelectScreen(void)
{
    update_all_preset_list_labels();
    update_preset_select_buttons();
}

// Clear a text area when it receives focus so the user can type without deleting the old value first.
static void textarea_focus_clear_cb(lv_event_t *e)
{
    lv_textarea_set_text((lv_obj_t *)lv_event_get_target(e), "");
}

// Populate ScreenPresetEdit labels and text areas for the current edit_preset.
extern "C" void InitPresetEditScreen(void)
{
    update_preset_edit_labels();

    // Register focus-to-clear on each editable text area (registered once at startup).
    if (ui_InsertNameTextArea != NULL) {
        lv_obj_add_event_cb(ui_InsertNameTextArea, textarea_focus_clear_cb, LV_EVENT_FOCUSED, NULL);
    }
    if (ui_InsertM1TextArea != NULL) {
        lv_obj_add_event_cb(ui_InsertM1TextArea, textarea_focus_clear_cb, LV_EVENT_FOCUSED, NULL);
    }
    if (ui_InsertM2TextArea != NULL) {
        lv_obj_add_event_cb(ui_InsertM2TextArea, textarea_focus_clear_cb, LV_EVENT_FOCUSED, NULL);
    }
}

// Set edit_preset to the currently selected preset and refresh ScreenPresetEdit before showing it.
extern "C" void PreparePresetEditScreen(void)
{
    edit_preset = (selected_preset > 0) ? selected_preset : 1;
    update_preset_edit_labels();
}

// Toggle selection of the given preset number and refresh the button states.
extern "C" void HandlePresetSelect(int preset_number)
{
    selected_preset = (selected_preset == preset_number) ? 0 : preset_number;
    update_preset_select_buttons();
}

// Apply the selected preset's heights to available motor controllers (unavailable modules are skipped).
extern "C" void ApplyPreset(void)
{
    if (selected_preset < 1 || selected_preset > PRESET_COUNT) return;
    const int idx = selected_preset - 1;

    const bool m1_unavailable = (!motor1_connected || motor1_alarm_code > 0);
    const bool m2_unavailable = (!motor2_connected || motor2_alarm_code > 0);

    if (!m1_unavailable) {
        target_height_motor1_tenths_mm = parse_preset_height_tenths_mm(presets[idx].m1);
        mesh.sendBroadcast(get_motor1_target_height_tag() + String(":") + String(target_height_motor1_tenths_mm));
    }
    if (!m2_unavailable) {
        target_height_motor2_tenths_mm = parse_preset_height_tenths_mm(presets[idx].m2);
        mesh.sendBroadcast(get_motor2_target_height_tag() + String(":") + String(target_height_motor2_tenths_mm));
    }
    mark_target_heights_dirty(millis());
    update_home_target_height_labels();
}

// Read the text areas on ScreenPresetEdit and save their values to the current edit_preset.
extern "C" void SaveCurrentEditPreset(void)
{
    const int idx = edit_preset - 1;

    if (ui_InsertNameTextArea != NULL) {
        const char *name_text = lv_textarea_get_text(ui_InsertNameTextArea);
        if (name_text != NULL && strlen(name_text) > 0) {
            strncpy(presets[idx].name, name_text, PRESET_NAME_MAX - 1);
            presets[idx].name[PRESET_NAME_MAX - 1] = '\0';
        }
    }
    if (ui_InsertM1TextArea != NULL) {
        const char *m1_text = lv_textarea_get_text(ui_InsertM1TextArea);
        char m1_norm[16];
        normalize_preset_height((m1_text != NULL && strlen(m1_text) > 0) ? m1_text : "0,0", m1_norm, sizeof(m1_norm));
        snprintf(presets[idx].m1, PRESET_HEIGHT_MAX, "M1: %smm", m1_norm);
    }
    if (ui_InsertM2TextArea != NULL) {
        const char *m2_text = lv_textarea_get_text(ui_InsertM2TextArea);
        char m2_norm[16];
        normalize_preset_height((m2_text != NULL && strlen(m2_text) > 0) ? m2_text : "0,0", m2_norm, sizeof(m2_norm));
        snprintf(presets[idx].m2, PRESET_HEIGHT_MAX, "M2: %smm", m2_norm);
    }

    update_preset_edit_labels();
    update_all_preset_list_labels();
    save_presets_to_nvs();
}

// Reset the current edit_preset to the factory defaults and refresh both screens.
extern "C" void ResetCurrentEditPreset(void)
{
    const int idx = edit_preset - 1;
    strncpy(presets[idx].name, "PIMM Engineering", PRESET_NAME_MAX - 1);
    presets[idx].name[PRESET_NAME_MAX - 1] = '\0';
    strncpy(presets[idx].m1, "M1: 0,0mm", PRESET_HEIGHT_MAX - 1);
    presets[idx].m1[PRESET_HEIGHT_MAX - 1] = '\0';
    strncpy(presets[idx].m2, "M2: 0,0mm", PRESET_HEIGHT_MAX - 1);
    presets[idx].m2[PRESET_HEIGHT_MAX - 1] = '\0';
    update_preset_edit_labels();
    update_all_preset_list_labels();
    save_presets_to_nvs();
}

// Advance to the next preset for editing, wrapping from 4 back to 1.
extern "C" void GoToNextEditPreset(void)
{
    edit_preset = (edit_preset % PRESET_COUNT) + 1;
    update_preset_edit_labels();
}

// Step back to the previous preset for editing, wrapping from 1 back to 4.
extern "C" void GoToPreviousEditPreset(void)
{
    edit_preset = (edit_preset == 1) ? PRESET_COUNT : edit_preset - 1;
    update_preset_edit_labels();
}

// Return the stored target height for the currently selected motor controller.
static int get_selected_target_height_tenths_mm()
{
    return target_motor_controller_two ? target_height_motor2_tenths_mm : target_height_motor1_tenths_mm; // Choose the matching stored height.
}

// Update the stored target height for the currently selected motor controller.
static void set_selected_target_height_tenths_mm(int value)
{
    value = clamp_target_height_tenths_mm(value); // Keep every stored target height inside the allowed range.

    if (target_motor_controller_two) { // Write to MotorController2 when it is selected.
        target_height_motor2_tenths_mm = value; // Save the new height for MotorController2.
    } else {
        target_height_motor1_tenths_mm = value; // Save the new height for MotorController1.
    }
    mark_target_heights_dirty(millis());
}

// Broadcast the current target height to the selected motor controller.
static void send_target_height_message()
{
    if (sync_target_heights) { // Send both controller values while Sync is active.
        const String motor1_message = get_motor1_target_height_tag() + String(":") + String(target_height_motor1_tenths_mm); // Build the MotorController1 payload.
        const String motor2_message = get_motor2_target_height_tag() + String(":") + String(target_height_motor2_tenths_mm); // Build the MotorController2 payload.
        mesh.sendBroadcast(motor1_message); // Send the MotorController1 value to the mesh.
        mesh.sendBroadcast(motor2_message); // Send the MotorController2 value to the mesh.
        return; // Stop after sending both synchronized values.
    }

    const int target_height_tenths_mm = get_selected_target_height_tenths_mm(); // Read the stored value for the selected controller.
    const String message = get_selected_target_height_tag() + String(":") + String(target_height_tenths_mm); // Build the outgoing mesh payload.
    mesh.sendBroadcast(message); // Send the target height payload to the selected controller only.
}

// Increase or decrease the target height in 0.1 mm steps and forward the new value.
static void handle_encoder_turn_message(const String &direction)
{
    // Ignore encoder turns when the selected module is disconnected or has an active alarm.
    const bool selected_unavailable = target_motor_controller_two
        ? (!motor2_connected || motor2_alarm_code > 0)
        : (!motor1_connected || motor1_alarm_code > 0);
    if (selected_unavailable) return;

    if (direction == "CW") { // Treat clockwise rotation as an increase.
        set_selected_target_height_tenths_mm(get_selected_target_height_tenths_mm() + get_encoder_step_size_tenths_mm()); // Increase the selected target height by the active encoder step size.
        if (sync_target_heights) { // Keep the inactive controller aligned while Sync is active.
            mirror_selected_target_height_to_inactive_controller(); // Copy the new active value to the inactive controller.
        }
        update_home_target_height_labels(); // Refresh both labels so the screen shows the new values.
        send_target_height_message(); // Forward the updated value to the selected motor controller.
        return; // Stop after handling the clockwise step.
    }

    if (direction == "CCW") { // Treat counter-clockwise rotation as a decrease.
        set_selected_target_height_tenths_mm(get_selected_target_height_tenths_mm() - get_encoder_step_size_tenths_mm()); // Decrease the selected target height by the active encoder step size.
        if (sync_target_heights) { // Keep the inactive controller aligned while Sync is active.
            mirror_selected_target_height_to_inactive_controller(); // Copy the new active value to the inactive controller.
        }
        update_home_target_height_labels(); // Refresh both labels so the screen shows the new values.
        send_target_height_message(); // Forward the updated value to the selected motor controller.
    }
}

// Dispatch the encoder button action based on the active mode.
static void handle_encoder_button_message()
{
    if (encoder_knob_switches_module) {
        SetSelectedMotorController(target_motor_controller_two ? 0 : 1); // Toggle between MotorController1 and MotorController2.
    } else {
        ToggleStepSizeMode(); // Cycle between the 0.1 mm and 1.0 mm step sizes.
    }
}

// Store the actual height from MotorController1 and refresh the Home screen label.
static void handle_actual_height_motor1_message(int actual_tenths_mm)
{
    actual_height_motor1_tenths_mm = actual_tenths_mm;
    motor1_height_known = true;
    lvgl_port_lock(-1);
    update_home_actual_height_labels();
    lvgl_port_unlock();
}

// Store the actual height from MotorController2 and refresh the Home screen label.
static void handle_actual_height_motor2_message(int actual_tenths_mm)
{
    actual_height_motor2_tenths_mm = actual_tenths_mm;
    motor2_height_known = true;
    lvgl_port_lock(-1);
    update_home_actual_height_labels();
    lvgl_port_unlock();
}

// Apply the calibrated height of 86.0 mm to MotorController1's target and actual values and refresh the labels.
static void handle_calibration_done_motor1_message()
{
    target_height_motor1_tenths_mm = 860;
    actual_height_motor1_tenths_mm = 860;
    motor1_height_known = true;
    mark_target_heights_dirty(millis());
    lvgl_port_lock(-1);
    update_home_target_height_labels();
    update_home_actual_height_labels();
    lvgl_port_unlock();
}

// Apply the calibrated height of 86.0 mm to MotorController2's target and actual values and refresh the labels.
static void handle_calibration_done_motor2_message()
{
    target_height_motor2_tenths_mm = 860;
    actual_height_motor2_tenths_mm = 860;
    motor2_height_known = true;
    mark_target_heights_dirty(millis());
    lvgl_port_lock(-1);
    update_home_target_height_labels();
    update_home_actual_height_labels();
    lvgl_port_unlock();
}

// Parse one incoming mesh message and dispatch it to the right handler.
static void handle_mesh_message(uint32_t from, const String &msg)
{
    if (!msg.startsWith("touch:")) { // Route non-touch messages to the right handler.

        // Track which node is MotorController1 and MotorController2 so we can detect disconnections.
        const bool is_motor1 = msg.startsWith("ActualHeightMotor1:") || msg == "CalibratedMotor1";
        const bool is_motor2 = msg.startsWith("ActualHeightMotor2:") || msg == "CalibratedMotor2";

        if (is_motor1) {
            motor1_last_seen_ms = millis(); // Refresh the heartbeat timestamp for timeout detection.
            if (!motor1_connected || motor1_node_id != from) {
                const bool was_connected = motor1_connected;
                motor1_node_id = from;
                motor1_connected = true;
                lvgl_port_lock(-1);
                update_home_connection_labels();
                lvgl_port_unlock();
                if (!was_connected) {
                    // Motor controller rebooted: its target resets to 0. Resend the saved value so it drives to the correct position immediately.
                    mesh.sendBroadcast("TargetHeightMotor1:" + String(target_height_motor1_tenths_mm));
                }
            }
        } else if (is_motor2) {
            motor2_last_seen_ms = millis(); // Refresh the heartbeat timestamp for timeout detection.
            if (!motor2_connected || motor2_node_id != from) {
                const bool was_connected = motor2_connected;
                motor2_node_id = from;
                motor2_connected = true;
                lvgl_port_lock(-1);
                update_home_connection_labels();
                lvgl_port_unlock();
                if (!was_connected) {
                    // Motor controller rebooted: its target resets to 0. Resend the saved value so it drives to the correct position immediately.
                    mesh.sendBroadcast("TargetHeightMotor2:" + String(target_height_motor2_tenths_mm));
                }
            }
        }

        if (msg.startsWith("AlarmMotor1:")) {
            const int new_code = msg.substring(String("AlarmMotor1:").length()).toInt();
            if (new_code != motor1_alarm_code) {
                if (new_code > 0 && motor1_alarm_code == 0) {
                    // Alarm just activated: freeze the target at the last known actual height.
                    target_height_motor1_tenths_mm = actual_height_motor1_tenths_mm;
                    mark_target_heights_dirty(millis());
                }
                motor1_alarm_code = new_code;
                lvgl_port_lock(-1);
                update_alarm_display();
                update_home_target_height_labels();
                lvgl_port_unlock();
            }
            return;
        } else if (msg.startsWith("AlarmMotor2:")) {
            const int new_code = msg.substring(String("AlarmMotor2:").length()).toInt();
            if (new_code != motor2_alarm_code) {
                if (new_code > 0 && motor2_alarm_code == 0) {
                    // Alarm just activated: freeze the target at the last known actual height.
                    target_height_motor2_tenths_mm = actual_height_motor2_tenths_mm;
                    mark_target_heights_dirty(millis());
                }
                motor2_alarm_code = new_code;
                lvgl_port_lock(-1);
                update_alarm_display();
                update_home_target_height_labels();
                lvgl_port_unlock();
            }
            return;
        }

        if (msg.startsWith("encoder:turn:")) {
            handle_encoder_turn_message(msg.substring(String("encoder:turn:").length())); // Pass the turn direction text through.
        } else if (msg == "encoder:button:pressed") {
            handle_encoder_button_message(); // Run the placeholder encoder button handler.
        } else if (msg.startsWith("ActualHeightMotor1:")) {
            const int sep = msg.indexOf(':'); // Locate the separator between the tag and the height value.
            if (sep >= 0) { // Forward the height to the handler only when the message is well-formed.
                handle_actual_height_motor1_message(msg.substring(sep + 1).toInt());
            }
        } else if (msg.startsWith("ActualHeightMotor2:")) {
            const int sep = msg.indexOf(':'); // Locate the separator between the tag and the height value.
            if (sep >= 0) { // Forward the height to the handler only when the message is well-formed.
                handle_actual_height_motor2_message(msg.substring(sep + 1).toInt());
            }
        } else if (msg == "CalibratedMotor1") {
            handle_calibration_done_motor1_message(); // Apply the 86.0 mm calibrated position to MotorController1.
        } else if (msg == "CalibratedMotor2") {
            handle_calibration_done_motor2_message(); // Apply the 86.0 mm calibrated position to MotorController2.
        }

        return; // Stop after processing the non-touch message.
    }

    const int first_separator = msg.indexOf(':'); // Find the separator after the message type.
    const int second_separator = msg.indexOf(':', first_separator + 1); // Find the separator after the X coordinate.
    const int third_separator = msg.indexOf(':', second_separator + 1); // Find the separator after the Y coordinate.

    if (first_separator < 0 || second_separator < 0 || third_separator < 0) { // Reject malformed touch messages.
        return; // Stop if the touch message does not contain all required fields.
    }

    const int x = msg.substring(first_separator + 1, second_separator).toInt(); // Convert the X field to an integer.
    const int y = msg.substring(second_separator + 1, third_separator).toInt(); // Convert the Y field to an integer.
    const bool active = msg.substring(third_separator + 1).toInt() != 0; // Convert the pressed flag to a boolean.

    lvgl_port_lock(-1); // Lock LVGL state before updating the simulated touch.
    set_simulated_touch(x, y, active); // Store the new simulated touch values.
    lvgl_port_unlock(); // Release LVGL state after the update is complete.
}

#if ESP_PANEL_LCD_BUS_TYPE == ESP_PANEL_BUS_TYPE_RGB
// Flush LVGL draw data directly to the RGB panel.
static void lvgl_port_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p)
{
    panel->getLcd()->drawBitmap(area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_p);
    lv_disp_flush_ready(disp);
}
#else
// Flush LVGL draw data when the panel requires explicit completion callbacks.
static void lvgl_port_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p)
{
    panel->getLcd()->drawBitmap(area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_p);
}

// Callback used by the panel driver to notify LVGL that flushing is finished.
static bool notify_lvgl_flush_ready(void *user_ctx)
{
    lv_disp_drv_t *disp_driver = (lv_disp_drv_t *)user_ctx;
    lv_disp_flush_ready(disp_driver);
    return false;
}
#endif

#if ESP_PANEL_USE_LCD_TOUCH
// Read touch input for LVGL from the physical touch controller.
static void lvgl_port_tp_read(lv_indev_drv_t *indev, lv_indev_data_t *data)
{
    LV_UNUSED(indev); // The driver object is not used by this read callback.

    lvgl_port_lock(-1); // Lock LVGL state before reading the simulated touch fields.
    const bool has_simulated_touch = simulated_touch_active; // Capture whether a simulated touch is active.
    const int touch_x = simulated_touch_x; // Capture the stored simulated X coordinate.
    const int touch_y = simulated_touch_y; // Capture the stored simulated Y coordinate.
    lvgl_port_unlock(); // Release LVGL state after copying the simulated touch fields.

    if (has_simulated_touch) { // Report the simulated touch when one is active.
        data->state = LV_INDEV_STATE_PR; // Tell LVGL the pointer is currently pressed.
        data->point.x = touch_x; // Provide the simulated X coordinate.
        data->point.y = touch_y; // Provide the simulated Y coordinate.
        return; // Skip the physical touch controller while simulated input is active.
    }

    panel->getLcdTouch()->readData(); // Poll the physical touch controller.
    const bool touched = panel->getLcdTouch()->getTouchState(); // Read whether the panel is currently touched.

    if (!touched) { // Report a released pointer when no physical touch is detected.
        data->state = LV_INDEV_STATE_REL; // Tell LVGL the pointer is not pressed.
        return; // Stop after reporting the released state.
    }

    const TouchPoint point = panel->getLcdTouch()->getPoint(); // Read the physical touch coordinates.
    data->state = LV_INDEV_STATE_PR; // Tell LVGL the pointer is pressed.
    data->point.x = point.x; // Forward the physical X coordinate to LVGL.
    data->point.y = point.y; // Forward the physical Y coordinate to LVGL.
}
#endif

// FreeRTOS task that runs the LVGL event loop.
static void lvgl_task(void *arg)
{
    LV_UNUSED(arg);

    while (1) {
        lvgl_port_lock(-1);
        uint32_t task_delay_ms = lv_timer_handler();
        lvgl_port_unlock();

        if (task_delay_ms > LVGL_TASK_MAX_DELAY_MS) {
            task_delay_ms = LVGL_TASK_MAX_DELAY_MS;
        } else if (task_delay_ms < LVGL_TASK_MIN_DELAY_MS) {
            task_delay_ms = LVGL_TASK_MIN_DELAY_MS;
        }

        vTaskDelay(pdMS_TO_TICKS(task_delay_ms));
    }
}

// FreeRTOS task that continuously services painlessMesh networking.
static void mesh_update_task(void *arg)
{
    LV_UNUSED(arg);

    while (1) {
        mesh.update();

        // Mark a controller offline when no message has arrived within MOTOR_TIMEOUT_MS.
        // motor1_last_seen_ms == 0 means we have never heard from it; skip until first contact.
        const uint32_t now_ms = millis();
        if (motor1_connected && motor1_last_seen_ms != 0 && (now_ms - motor1_last_seen_ms) > MOTOR_TIMEOUT_MS) {
            motor1_connected = false;
            motor1_node_id = 0;
            target_height_motor1_tenths_mm = actual_height_motor1_tenths_mm; // Freeze target at last known position.
            mark_target_heights_dirty(now_ms);
            lvgl_port_lock(-1);
            update_home_connection_labels(); // Also refreshes borders.
            update_home_target_height_labels();
            lvgl_port_unlock();
        }
        if (motor2_connected && motor2_last_seen_ms != 0 && (now_ms - motor2_last_seen_ms) > MOTOR_TIMEOUT_MS) {
            motor2_connected = false;
            motor2_node_id = 0;
            target_height_motor2_tenths_mm = actual_height_motor2_tenths_mm; // Freeze target at last known position.
            mark_target_heights_dirty(now_ms);
            lvgl_port_lock(-1);
            update_home_connection_labels(); // Also refreshes borders.
            update_home_target_height_labels();
            lvgl_port_unlock();
        }

        // Save target heights to NVS after the debounce period if they have changed.
        if (target_heights_dirty && (now_ms - target_heights_last_changed_ms) >= TARGET_HEIGHT_SAVE_DEBOUNCE_MS) {
            save_target_heights_to_nvs();
            target_heights_dirty = false;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// Set up the display panel, LVGL, and the touch/expander hardware.
static void init_panel_and_lvgl()
{
    panel = new ESP_Panel();

    lv_init();

    static lv_disp_draw_buf_t draw_buf;
    uint8_t *buf = (uint8_t *)heap_caps_calloc(1, LVGL_BUF_SIZE * sizeof(lv_color_t), MALLOC_CAP_INTERNAL);
    assert(buf != NULL);
    lv_disp_draw_buf_init(&draw_buf, buf, NULL, LVGL_BUF_SIZE);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = ESP_PANEL_LCD_H_RES;
    disp_drv.ver_res = ESP_PANEL_LCD_V_RES;
    disp_drv.flush_cb = lvgl_port_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

#if ESP_PANEL_USE_LCD_TOUCH
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = lvgl_port_tp_read;
    lv_indev_drv_register(&indev_drv);
#endif

    panel->init();
#if ESP_PANEL_LCD_BUS_TYPE != ESP_PANEL_BUS_TYPE_RGB
    panel->getLcd()->setCallback(notify_lvgl_flush_ready, &disp_drv);
#endif

    ESP_IOExpander *expander = new ESP_IOExpander_CH422G(I2C_MASTER_NUM, ESP_IO_EXPANDER_I2C_CH422G_ADDRESS_000);
    expander->init();
    expander->begin();
    expander->multiPinMode(TP_RST | LCD_BL | LCD_RST | SD_CS | USB_SEL, OUTPUT);
    expander->multiDigitalWrite(TP_RST | LCD_BL | LCD_RST | SD_CS, HIGH);
    expander->digitalWrite(USB_SEL, LOW);

    panel->addIOExpander(expander);
    panel->begin();
}

// Configure the mesh network.
static void init_mesh()
{
    mesh.setDebugMsgTypes(ERROR | STARTUP); // Keep mesh logging limited to startup and errors.
    mesh.onReceive(handle_mesh_message); // Register the callback that processes incoming DisplayInput messages.
    mesh.init(MESH_PREFIX, MESH_PASSWORD, &userScheduler, MESH_PORT); // Start the mesh network with the shared credentials.
}

// Arduino startup entry point.
void setup()
{
    Serial.begin(115200);
    delay(100);

    lvgl_mux = xSemaphoreCreateRecursiveMutex();
    assert(lvgl_mux != NULL);

    init_panel_and_lvgl();

    lvgl_port_lock(-1);
    ui_init();
    lvgl_port_unlock();

    load_settings_from_nvs(); // Restore encoder step size and knob mode from NVS.
    load_target_heights_from_nvs(); // Restore last target heights from NVS.
    SetSelectedMotorController(0); // Start with MotorController1 selected and highlight its button.
    update_home_target_height_labels(); // Show both initial target heights on the Home screen.
    update_home_connection_labels(); // Both controllers start as disconnected (Red) until the mesh reports them.
    update_alarm_display(); // Both alarm indicators start hidden and reset buttons start LightBlue.
    update_encoder_mode_buttons(); // WisselenVanModule starts Green (active); AanpassenVanDeStap starts LightBlue.
    load_presets_from_nvs(); // Restore any presets saved before the last power-down.
    InitPresetsSelectScreen(); // Populate preset labels and set initial button states.
    InitPresetEditScreen(); // Populate the edit screen labels for the default edit_preset.

    init_mesh();

    xTaskCreate(lvgl_task, "lvgl", LVGL_TASK_STACK_SIZE, NULL, LVGL_TASK_PRIORITY, NULL);
    xTaskCreate(mesh_update_task, "mesh_update", MESH_TASK_STACK_SIZE, NULL, MESH_TASK_PRIORITY, NULL);

    Serial.println("Setup done: LVGL + Mesh active");
}

// Arduino loop stays alive while the real work happens in FreeRTOS tasks.
void loop()
{
    vTaskDelay(pdMS_TO_TICKS(1000));
}

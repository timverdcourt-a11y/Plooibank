#include <Arduino.h>
#include <lvgl.h>
#include <painlessMesh.h>
#include <ESP_Panel_Library.h>
#include <ESP_IOExpander_Library.h>
#include <ui.h>

// Extend IO pin define
#define TP_RST 1
#define LCD_BL 2
#define LCD_RST 3
#define SD_CS 4
#define USB_SEL 5

// I2C pin define
#define I2C_MASTER_NUM 0

// Mesh network configuration (kept aligned with UserController)
#define MESH_PREFIX "PIMM"
#define MESH_PASSWORD "Odisee1234"
#define MESH_PORT 5555

// LVGL porting configurations
#define LVGL_TICK_PERIOD_MS (2)
#define LVGL_TASK_MAX_DELAY_MS (500)
#define LVGL_TASK_MIN_DELAY_MS (1)
#define LVGL_TASK_STACK_SIZE (4 * 1024)
#define LVGL_TASK_PRIORITY (2)
#define LVGL_BUF_SIZE (ESP_PANEL_LCD_H_RES * 20)

#define MESH_TASK_STACK_SIZE (6 * 1024)
#define MESH_TASK_PRIORITY (2)
#define MESH_TX_TASK_STACK_SIZE (4 * 1024)
#define MESH_TX_TASK_PRIORITY (1)

struct MeshState {
    int module1HeightTenthMm;
    int module2HeightTenthMm;
    int target1TenthMm;
    int target2TenthMm;
    bool syncEnabled;
    uint32_t lastRxMs;
    uint32_t lastFromNode;
};

struct SimulatedTouch {
    int x;
    int y;
    bool pressed;
    uint32_t lastUpdateMs;
};

static ESP_Panel *panel = NULL;
static SemaphoreHandle_t lvgl_mux = NULL;
static SemaphoreHandle_t mesh_state_mutex = NULL;
static QueueHandle_t ui_update_queue = NULL;

static Scheduler userScheduler;
static painlessMesh mesh;

// Compatibility with generated SquareLine screen code (ui_ScreenHome.c).
int module1Height = 0;

static MeshState g_mesh_state = {
    .module1HeightTenthMm = 0,
    .module2HeightTenthMm = 0,
    .target1TenthMm = 0,
    .target2TenthMm = 0,
    .syncEnabled = false,
    .lastRxMs = 0,
    .lastFromNode = 0,
};

static SimulatedTouch g_simulated_touch = {
    .x = 0,
    .y = 0,
    .pressed = false,
    .lastUpdateMs = 0,
};

static void lvgl_port_lock(int timeout_ms)
{
    const TickType_t timeout_ticks = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    xSemaphoreTakeRecursive(lvgl_mux, timeout_ticks);
}

static void lvgl_port_unlock(void)
{
    xSemaphoreGiveRecursive(lvgl_mux);
}

static void format_mm_text(char *out, size_t out_size, int tenth_mm)
{
    const int whole = tenth_mm / 10;
    const int frac = abs(tenth_mm % 10);
    snprintf(out, out_size, "%02d.%01d mm", whole, frac);
}

static void apply_mesh_state_to_ui(const MeshState &state)
{
    char text_buf[32];

    if (ui_LblMod1Height != NULL) {
        format_mm_text(text_buf, sizeof(text_buf), state.module1HeightTenthMm);
        lv_label_set_text(ui_LblMod1Height, text_buf);
    }

    if (ui_LblMod2Height != NULL) {
        format_mm_text(text_buf, sizeof(text_buf), state.module2HeightTenthMm);
        lv_label_set_text(ui_LblMod2Height, text_buf);
    }

    if (ui_LblMod1Tgt != NULL) {
        snprintf(text_buf, sizeof(text_buf), "Doel: %d.%01d mm", state.target1TenthMm / 10, abs(state.target1TenthMm % 10));
        lv_label_set_text(ui_LblMod1Tgt, text_buf);
    }

    if (ui_LblMod2Tgt != NULL) {
        snprintf(text_buf, sizeof(text_buf), "Doel: %d.%01d mm", state.target2TenthMm / 10, abs(state.target2TenthMm % 10));
        lv_label_set_text(ui_LblMod2Tgt, text_buf);
    }

    if (ui_LblSyncStatus != NULL) {
        lv_label_set_text(ui_LblSyncStatus, state.syncEnabled ? "Sync: AAN" : "Sync: UIT");
    }

    if (ui_HomeBtn3Text != NULL) {
        lv_label_set_text(ui_HomeBtn3Text, state.syncEnabled ? "Sync: AAN" : "Sync: UIT");
    }

    if (ui_LblProgInfo != NULL) {
        snprintf(text_buf, sizeof(text_buf), "RX node: %u", static_cast<unsigned>(state.lastFromNode));
        lv_label_set_text(ui_LblProgInfo, text_buf);
    }
}

static void push_ui_update_from_state()
{
    MeshState snapshot;
    xSemaphoreTake(mesh_state_mutex, portMAX_DELAY);
    snapshot = g_mesh_state;
    xSemaphoreGive(mesh_state_mutex);

    xQueueOverwrite(ui_update_queue, &snapshot);
}

static bool parse_two_ints_csv(const String &msg, int &a, int &b)
{
    const int comma = msg.indexOf(',');
    if (comma <= 0 || comma >= (msg.length() - 1)) {
        return false;
    }

    const String left = msg.substring(0, comma);
    const String right = msg.substring(comma + 1);

    bool left_ok = true;
    bool right_ok = true;

    for (unsigned int i = 0; i < left.length(); ++i) {
        if (!isDigit(left[i]) && !(i == 0 && left[i] == '-')) {
            left_ok = false;
            break;
        }
    }

    for (unsigned int i = 0; i < right.length(); ++i) {
        if (!isDigit(right[i]) && !(i == 0 && right[i] == '-')) {
            right_ok = false;
            break;
        }
    }

    if (!left_ok || !right_ok) {
        return false;
    }

    a = left.toInt();
    b = right.toInt();
    return true;
}

static void received_callback(uint32_t from, String &msg)
{
    bool state_changed = false;

    xSemaphoreTake(mesh_state_mutex, portMAX_DELAY);
    g_mesh_state.lastFromNode = from;
    g_mesh_state.lastRxMs = millis();

    // Expected format from modules: device:1:height:XXX or device:2:height:XXX
    if (msg.startsWith("device:1:height:")) {
        g_mesh_state.module1HeightTenthMm = msg.substring(16).toInt();
        module1Height = g_mesh_state.module1HeightTenthMm;
        state_changed = true;
    } else if (msg.startsWith("device:2:height:")) {
        g_mesh_state.module2HeightTenthMm = msg.substring(16).toInt();
        state_changed = true;
    } else if (msg.startsWith("sync:")) {
        g_mesh_state.syncEnabled = (msg.substring(5).toInt() != 0);
        state_changed = true;
    } else if (msg.startsWith("touch:")) {
        // Format: "touch:X:Y:pressed" where pressed is 1 for press, 0 for release
        Serial.printf(">>> TOUCH MSG RECEIVED: '%s' (len=%d)\n", msg.c_str(), msg.length());
        
        int first_colon = msg.indexOf(':', 6);
        int second_colon = msg.indexOf(':', first_colon + 1);
        
        Serial.printf(">>> TOUCH PARSING: first_colon=%d, second_colon=%d\n", first_colon, second_colon);
        
        if (first_colon > 0 && second_colon > first_colon) {
            String x_str = msg.substring(6, first_colon);
            String y_str = msg.substring(first_colon + 1, second_colon);
            String pressed_str = msg.substring(second_colon + 1);
            
            Serial.printf(">>> TOUCH PARSED: x_str='%s', y_str='%s', pressed_str='%s'\n", 
                         x_str.c_str(), y_str.c_str(), pressed_str.c_str());
            
            int x = x_str.toInt();
            int y = y_str.toInt();
            bool pressed = (pressed_str.toInt() != 0);
            
            // Update simulated touch state
            g_simulated_touch.x = x;
            g_simulated_touch.y = y;
            g_simulated_touch.pressed = pressed;
            g_simulated_touch.lastUpdateMs = millis();
            
            Serial.printf(">>> TOUCH STATE UPDATED: X=%d Y=%d Pressed=%d\n", x, y, pressed);
        } else {
            Serial.printf(">>> TOUCH PARSE FAILED: first_colon=%d, second_colon=%d\n", first_colon, second_colon);
        }
    } else {
        // Compatibility with UserController command format: "target1,target2"
        int t1 = 0;
        int t2 = 0;
        if (parse_two_ints_csv(msg, t1, t2)) {
            g_mesh_state.target1TenthMm = t1;
            g_mesh_state.target2TenthMm = t2;
            state_changed = true;
        }
    }

    xSemaphoreGive(mesh_state_mutex);

    if (state_changed) {
        push_ui_update_from_state();
    }

    Serial.printf("Mesh RX from %u: %s\n", static_cast<unsigned>(from), msg.c_str());
}

static void send_control_targets()
{
    MeshState snapshot;
    xSemaphoreTake(mesh_state_mutex, portMAX_DELAY);
    snapshot = g_mesh_state;
    xSemaphoreGive(mesh_state_mutex);

    const String msg = String(snapshot.target1TenthMm) + "," + String(snapshot.target2TenthMm);
    mesh.sendBroadcast(msg);
    Serial.println("Mesh TX control: " + msg);
}

#if ESP_PANEL_LCD_BUS_TYPE == ESP_PANEL_BUS_TYPE_RGB
static void lvgl_port_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p)
{
    panel->getLcd()->drawBitmap(area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_p);
    lv_disp_flush_ready(disp);
}
#else
static void lvgl_port_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p)
{
    panel->getLcd()->drawBitmap(area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_p);
}

static bool notify_lvgl_flush_ready(void *user_ctx)
{
    lv_disp_drv_t *disp_driver = (lv_disp_drv_t *)user_ctx;
    lv_disp_flush_ready(disp_driver);
    return false;
}
#endif

#if ESP_PANEL_USE_LCD_TOUCH
static void lvgl_port_tp_read(lv_indev_drv_t *indev, lv_indev_data_t *data)
{
    LV_UNUSED(indev);

    // Check simulated touch first (from button input)
    // Make a snapshot with mutex protection
    bool simulated_pressed = false;
    int simulated_x = 0;
    int simulated_y = 0;
    
    xSemaphoreTake(mesh_state_mutex, portMAX_DELAY);
    simulated_pressed = g_simulated_touch.pressed;
    simulated_x = g_simulated_touch.x;
    simulated_y = g_simulated_touch.y;
    xSemaphoreGive(mesh_state_mutex);
    
    if (simulated_pressed) {
        data->state = LV_INDEV_STATE_PR;
        data->point.x = simulated_x;
        data->point.y = simulated_y;
        Serial.printf("LVGL_TP_READ: Simulated touch ACTIVE at X=%d Y=%d\n", data->point.x, data->point.y);
    } else {
        // Fall back to hardware touch
        panel->getLcdTouch()->readData();
        const bool touched = panel->getLcdTouch()->getTouchState();

        if (!touched) {
            data->state = LV_INDEV_STATE_REL;
        } else {
            const TouchPoint point = panel->getLcdTouch()->getPoint();
            data->state = LV_INDEV_STATE_PR;
            data->point.x = point.x;
            data->point.y = point.y;
            Serial.printf("LVGL_TP_READ: Hardware touch at X=%d Y=%d\n", data->point.x, data->point.y);
        }
    }
}
#endif

static void lvgl_task(void *arg)
{
    LV_UNUSED(arg);

    uint32_t task_delay_ms = LVGL_TASK_MAX_DELAY_MS;
    MeshState pending;

    while (1) {
        lvgl_port_lock(-1);

        while (xQueueReceive(ui_update_queue, &pending, 0) == pdTRUE) {
            apply_mesh_state_to_ui(pending);
        }

        task_delay_ms = lv_timer_handler();

        lvgl_port_unlock();

        if (task_delay_ms > LVGL_TASK_MAX_DELAY_MS) {
            task_delay_ms = LVGL_TASK_MAX_DELAY_MS;
        } else if (task_delay_ms < LVGL_TASK_MIN_DELAY_MS) {
            task_delay_ms = LVGL_TASK_MIN_DELAY_MS;
        }

        vTaskDelay(pdMS_TO_TICKS(task_delay_ms));
    }
}

static void mesh_update_task(void *arg)
{
    LV_UNUSED(arg);

    while (1) {
        mesh.update();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void mesh_tx_task(void *arg)
{
    LV_UNUSED(arg);

    TickType_t last_wake = xTaskGetTickCount();
    while (1) {
        // Periodic broadcast keeps this node active as a controller and sends target values.
        send_control_targets();
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(2000));
    }
}

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

static void init_mesh()
{
    mesh.setDebugMsgTypes(ERROR | STARTUP);
    mesh.init(MESH_PREFIX, MESH_PASSWORD, &userScheduler, MESH_PORT);
    mesh.onReceive(&received_callback);
}

void setup()
{
    Serial.begin(115200);
    delay(100);

    lvgl_mux = xSemaphoreCreateRecursiveMutex();
    mesh_state_mutex = xSemaphoreCreateMutex();
    ui_update_queue = xQueueCreate(1, sizeof(MeshState));

    assert(lvgl_mux != NULL);
    assert(mesh_state_mutex != NULL);
    assert(ui_update_queue != NULL);

    init_panel_and_lvgl();

    lvgl_port_lock(-1);
    ui_init();
    apply_mesh_state_to_ui(g_mesh_state);
    lvgl_port_unlock();

    init_mesh();

    xTaskCreate(lvgl_task, "lvgl", LVGL_TASK_STACK_SIZE, NULL, LVGL_TASK_PRIORITY, NULL);
    xTaskCreate(mesh_update_task, "mesh_update", MESH_TASK_STACK_SIZE, NULL, MESH_TASK_PRIORITY, NULL);
    xTaskCreate(mesh_tx_task, "mesh_tx", MESH_TX_TASK_STACK_SIZE, NULL, MESH_TX_TASK_PRIORITY, NULL);

    Serial.println("Setup done: LVGL + FreeRTOS + Mesh active");
}

void loop()
{
    // Main application runs fully in FreeRTOS tasks.
    vTaskDelay(pdMS_TO_TICKS(1000));
}

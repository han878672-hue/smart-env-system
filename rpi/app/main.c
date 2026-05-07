#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <mosquitto.h>
#include <pthread.h>
#include <lgpio.h>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>

static uint16_t color565(uint8_t r, uint8_t g, uint8_t b)
{
    return ((r & 0xF8) << 8) |
           ((g & 0xFC) << 3) |
           (b >> 3);
}

#define UART_DEV "/dev/ttyAMA2"
#define DEVICE_ID "rpi_env01"
#define SPI_DEV   "/dev/spidev0.0"

#define COLOR_BLUE color565(91,155,213)
#define COLOR_GREEN color565(46,204,113)
#define COLOR_RED color565(231,76,60)
#define COLOR_ORANGE color565(240,165,0)

// LCD GPIO
#define PIN_DC   25
#define PIN_RST  24

// ST7735 尺寸
#define LCD_W 128
#define LCD_H 128

// ===== 共享資料 =====
int motion = 0;
int temp = 0, hum = 0, air = 0;
int data_ready = 0;

time_t last_uart_time = 0;
int uart_initialized = 0;

pthread_mutex_t lock;

// ===== SPI/LCD 全域 =====
int spi_fd;
int gpio_h;

// ===== 顏色定義 (RGB565) =====
#define COLOR_BLACK   color565(0,0,0)
#define COLOR_WHITE   color565(255,255,255)
#define COLOR_BLUE    color565(91,155,213)
#define COLOR_YELLOW  color565(255, 215, 0)
#define COLOR_ORANGE  color565(240,165,0)
#define COLOR_GREEN   color565(46,204,113)
#define COLOR_RED     color565(231,76,60)
#define COLOR_DARKBG  color565(13,27,42)
#define COLOR_TITLEBG color565(26,26,46)
#define COLOR_GRAY    color565(170,170,170)

// ===== SPI 傳輸 =====
static void spi_write(const uint8_t *buf, int len)
{
    struct spi_ioc_transfer tr = {
        .tx_buf = (unsigned long)buf,
        .len    = len,
        .speed_hz = 80000000,
        .bits_per_word = 8,
    };
    ioctl(spi_fd, SPI_IOC_MESSAGE(1), &tr);
}

static void lcd_cmd(uint8_t cmd)
{
    lgGpioWrite(gpio_h, PIN_DC, 0);
    spi_write(&cmd, 1);
}

static void lcd_data(uint8_t data)
{
    lgGpioWrite(gpio_h, PIN_DC, 1);
    spi_write(&data, 1);
}

static void lcd_data16(uint16_t data)
{
    uint8_t buf[2] = { data >> 8, data & 0xFF };
    lgGpioWrite(gpio_h, PIN_DC, 1);
    spi_write(buf, 2);
}

// ===== ST7735 初始化 =====
static void lcd_init()
{
    spi_fd = open(SPI_DEV, O_RDWR);

    uint8_t mode = SPI_MODE_0;
    ioctl(spi_fd, SPI_IOC_WR_MODE, &mode);

    gpio_h = lgGpiochipOpen(0);
    lgGpioClaimOutput(gpio_h, 0, PIN_DC,  0);
    lgGpioClaimOutput(gpio_h, 0, PIN_RST, 0);

    // Reset
    lgGpioWrite(gpio_h, PIN_RST, 0);
    usleep(100000);
    lgGpioWrite(gpio_h, PIN_RST, 1);
    usleep(100000);

    lcd_cmd(0x01); // SW reset
    usleep(150000);

    lcd_cmd(0x11); // sleep out
    usleep(150000);

    // Frame rate
    lcd_cmd(0xB1);
    lcd_data(0x01); lcd_data(0x2C); lcd_data(0x2D);

    // Power control
    lcd_cmd(0xC0);
    lcd_data(0xA2); lcd_data(0x02); lcd_data(0x84);

    lcd_cmd(0xC1);
    lcd_data(0xC5);

    lcd_cmd(0xC2);
    lcd_data(0x0A); lcd_data(0x00);

    lcd_cmd(0xC3);
    lcd_data(0x8A); lcd_data(0x2A);

    lcd_cmd(0xC4);
    lcd_data(0x8A); lcd_data(0xEE);

    lcd_cmd(0xC5);
    lcd_data(0x0E);

    // 方向+顏色
    lcd_cmd(0x36);
    lcd_data(0x68);   

    lcd_cmd(0x3A);
    lcd_data(0x05);   // 16-bit

    // Gamma
    lcd_cmd(0xE0);
    lcd_data(0x0F); lcd_data(0x1A); lcd_data(0x0F); lcd_data(0x18);

    lcd_cmd(0xE1);
    lcd_data(0x0F); lcd_data(0x1B); lcd_data(0x0F); lcd_data(0x17);

    lcd_cmd(0x29); // display on
    usleep(100000);
}

// ===== 畫矩形 =====
static void lcd_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    lcd_cmd(0x2A);
    lcd_data(0); lcd_data(x);
    lcd_data(0); lcd_data(x + w - 1);
    lcd_cmd(0x2B);
    lcd_data(0); lcd_data(y);
    lcd_data(0); lcd_data(y + h - 1);
    lcd_cmd(0x2C);

    lgGpioWrite(gpio_h, PIN_DC, 1);
    uint8_t buf[2] = { color >> 8, color & 0xFF };
    for (int i = 0; i < w * h; i++){
        spi_write(buf, 2);

    }	
}

// ===== 畫字元 (5x7 font) =====
static const uint8_t font5x7[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, // ' '
    {0x3E,0x51,0x49,0x45,0x3E}, // '0'
    {0x00,0x42,0x7F,0x40,0x00}, // '1'
    {0x42,0x61,0x51,0x49,0x46}, // '2'
    {0x21,0x41,0x45,0x4B,0x31}, // '3'
    {0x18,0x14,0x12,0x7F,0x10}, // '4'
    {0x27,0x45,0x45,0x45,0x39}, // '5'
    {0x3C,0x4A,0x49,0x49,0x30}, // '6'
    {0x01,0x71,0x09,0x05,0x03}, // '7'
    {0x36,0x49,0x49,0x49,0x36}, // '8'
    {0x06,0x49,0x49,0x29,0x1E}, // '9'
    {0x7C,0x12,0x11,0x12,0x7C}, // 'A' (index 11)
    {0x7F,0x49,0x49,0x49,0x36}, // 'B'
    {0x3E,0x41,0x41,0x41,0x22}, // 'C'
    {0x7F,0x41,0x41,0x22,0x1C}, // 'D'
    {0x7F,0x49,0x49,0x49,0x41}, // 'E'
    {0x7F,0x09,0x09,0x09,0x01}, // 'F'
    {0x3E,0x41,0x49,0x49,0x7A}, // 'G'
    {0x7F,0x08,0x08,0x08,0x7F}, // 'H'
    {0x00,0x41,0x7F,0x41,0x00}, // 'I'
    {0x20,0x40,0x41,0x3F,0x01}, // 'J'
    {0x7F,0x08,0x14,0x22,0x41}, // 'K'
    {0x7F,0x40,0x40,0x40,0x40}, // 'L'
    {0x7F,0x02,0x0C,0x02,0x7F}, // 'M'
    {0x7F,0x04,0x08,0x10,0x7F}, // 'N'
    {0x3E,0x41,0x41,0x41,0x3E}, // 'O'
    {0x7F,0x09,0x09,0x09,0x06}, // 'P'
    {0x3E,0x41,0x51,0x21,0x5E}, // 'Q'
    {0x7F,0x09,0x19,0x29,0x46}, // 'R'
    {0x46,0x49,0x49,0x49,0x31}, // 'S'
    {0x01,0x01,0x7F,0x01,0x01}, // 'T'
    {0x3F,0x40,0x40,0x40,0x3F}, // 'U'
    {0x1F,0x20,0x40,0x20,0x1F}, // 'V'
    {0x3F,0x40,0x38,0x40,0x3F}, // 'W'
    {0x63,0x14,0x08,0x14,0x63}, // 'X'
    {0x07,0x08,0x70,0x08,0x07}, // 'Y'
    {0x61,0x51,0x49,0x45,0x43}, // 'Z'
    {0x00,0x36,0x36,0x00,0x00}, // ':'
    {0x00,0x60,0x60,0x00,0x00}, // '.'
    {0x08,0x08,0x3E,0x08,0x08}, // '+'
    {0x62,0x64,0x08,0x13,0x23}, // '%'
};

static int char_index(char c)
{
    if (c == ' ') return 0;
    if (c >= '0' && c <= '9') return c - '0' + 1;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 11;
    if (c == ':') return 37;
    if (c == '.') return 38;
    if (c == '+') return 39;
    if (c == '%') return 40;
    return 0;
}

static void lcd_draw_char(int x, int y, char c, uint16_t fg, uint16_t bg)
{
    int idx = char_index(c);
    for (int col = 0; col < 5; col++) {
        uint8_t line = font5x7[idx][col];
        for (int row = 0; row < 7; row++) {
            uint16_t color = (line & (1 << row)) ? fg : bg;
            lcd_fill_rect(x + col, y + row, 1, 1, color);
        }
    }
    lcd_fill_rect(x + 5, y, 1, 7, bg);
}

static void lcd_draw_string(int x, int y, const char *str, uint16_t fg, uint16_t bg)
{
    while (*str) {
        char upper = (*str >= 'a' && *str <= 'z') ? *str - 32 : *str;
        lcd_draw_char(x, y, upper, fg, bg);
        x += 6;
        str++;
    }
}

// ===== 畫圓點 =====
static void lcd_draw_dot(int cx, int cy, int r, uint16_t color)
{
    lcd_fill_rect(cx - r, cy - r, r * 2, r * 2, color);
}

// ===== LCD 顯示畫面 =====
static void lcd_draw_screen(int t, int h, int a, int occ, int disconnected)
{
    // 標題列背景
    //lcd_fill_rect(0, 0, 128, 12, COLOR_TITLEBG);
    lcd_fill_rect(0, 0, 128, 12, COLOR_BLACK	);
    lcd_draw_string(3,  2, DEVICE_ID, COLOR_GRAY,  COLOR_TITLEBG);

    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char time_str[9];
    strftime(time_str, sizeof(time_str), "%H:%M:%S", tm_info);
    lcd_draw_string(74, 2, time_str, COLOR_GRAY, COLOR_TITLEBG);

    // 四個區塊
    // Temperature
    lcd_fill_rect(1,  14, 62, 54, COLOR_DARKBG);
    lcd_draw_string(4, 16, "TEMP", COLOR_BLUE, COLOR_DARKBG);

    if (disconnected)
    lcd_draw_string(4, 44, "--", COLOR_WHITE, COLOR_DARKBG);
    else{
    char val[16];
    snprintf(val, sizeof(val), "%d", t);
    lcd_draw_string(4, 44, val, COLOR_WHITE, COLOR_DARKBG);
    }
    lcd_draw_string(22, 50, "C", COLOR_BLUE, COLOR_DARKBG);

    // Humidity
    lcd_fill_rect(65, 14, 62, 54, COLOR_DARKBG);
    lcd_draw_string(68, 16, "HUM", COLOR_BLUE, COLOR_DARKBG);

    if (disconnected)
    lcd_draw_string(68, 44, "--", COLOR_WHITE, COLOR_DARKBG);
    else{
    char val[16];
    snprintf(val, sizeof(val), "%d", h);
    lcd_draw_string(68, 44, val, COLOR_WHITE, COLOR_DARKBG);
    }
    lcd_draw_string(86, 50, "%", COLOR_BLUE, COLOR_DARKBG);
    

    // Air Quality
    lcd_fill_rect(1,  70, 62, 45, COLOR_DARKBG);
    lcd_draw_string(4, 72, "AIR", COLOR_BLUE, COLOR_DARKBG);

    if (disconnected) {
        lcd_draw_string(4, 85, "--", COLOR_WHITE, COLOR_DARKBG);
        lcd_draw_string(4, 100, "    ", COLOR_GRAY, COLOR_DARKBG);
    } else {
        char val[16];
        snprintf(val, sizeof(val), "%d", a);
        uint16_t air_color;
        const char *air_label;
        if (a < 300) {
            air_color = COLOR_GREEN;
            air_label = "GOOD  ";
        } else if (a <= 600) {
            air_color = COLOR_ORANGE;
            air_label = "NORMAL";
        } else {
            air_color = COLOR_RED;
            air_label = "BAD   ";
        }
        lcd_draw_string(4, 85, val, air_color, COLOR_DARKBG);
        lcd_draw_string(4, 100, air_label, air_color, COLOR_DARKBG);
    }

    // Occupancy
    lcd_fill_rect(65, 70, 62, 45, COLOR_DARKBG);
    lcd_draw_string(68, 72, "OCCUPANCY", COLOR_BLUE, COLOR_DARKBG);
    uint16_t dot_color = occ ? COLOR_GREEN : COLOR_RED;
    const char *occ_str = occ ? "true" : "false";
    lcd_draw_dot(71, 92, 3, dot_color);
    lcd_draw_string(68, 100, occ_str, dot_color, COLOR_DARKBG);

    // 底部列
    lcd_fill_rect(0, 116, 128, 12, 0x1082);

    if (disconnected)
    lcd_draw_string(22, 118, "NO SIGNAL", COLOR_RED, 0x1082);
    else{
    lcd_draw_string(22, 118, "UPDATED 2S AGO", COLOR_GRAY, 0x1082);
    }
}

// ===== LCD thread =====
void *lcd_thread(void *arg)
{
    lcd_fill_rect(0,0,128,128,COLOR_RED);
    sleep(1);
    lcd_fill_rect(0,0,128,128,COLOR_GREEN);
    sleep(1);
    lcd_fill_rect(0,0,128,128,COLOR_BLUE);
    sleep(1);

    lcd_init();
    lcd_fill_rect(0, 0, 128, 128, COLOR_BLACK);

    while (1)
    {
        pthread_mutex_lock(&lock);
        int m = motion;
        int t = temp;
        int h = hum;
        int a = air;
        pthread_mutex_unlock(&lock);

	time_t now = time(NULL);
        int disconnected = (now - last_uart_time > 5);

        lcd_draw_screen(t, h, a, m, disconnected);
    	sleep(2);
    }
}

// ===== PIR thread =====
void *pir_thread(void *arg)
{
    int pir_fd = open("/dev/pir_dev", O_RDONLY);
    char pir_buf[10];

    int last_raw = 0;
    int stable_count = 0;
    int state = 0;

    time_t falling_edge_time = 0;
    int gpio_was_high = 0;

    time_t start_time = time(NULL);

    while (1)
    {
        memset(pir_buf, 0, sizeof(pir_buf));
        read(pir_fd, pir_buf, sizeof(pir_buf));

        int curr = (pir_buf[0] == '1') ? 1 : 0;
        time_t now = time(NULL);

        if (now - start_time < 5)
        {
            motion = 0;
            usleep(100000);
            continue;
        }

        if (curr == last_raw)
            stable_count++;
        else
            stable_count = 0;
        last_raw = curr;

        if (stable_count < 2)
        {
            usleep(100000);
            continue;
        }

        if (curr == 1 && gpio_was_high == 0)
        {
            gpio_was_high = 1;
            falling_edge_time = 0;
        }

        if (curr == 0 && gpio_was_high == 1)
        {
            gpio_was_high = 0;
            falling_edge_time = now;
        }

        if (curr == 1)
        {
            state = 1;
        }
        else if (falling_edge_time > 0 && (now - falling_edge_time < 2))
        {
            state = 1;
        }
        else
        {
            state = 0;
        }

        pthread_mutex_lock(&lock);
        motion = state;
        pthread_mutex_unlock(&lock);

        usleep(100000);
    }
}

// ===== UART thread =====
void *uart_thread(void *arg)
{
    int uart_fd = open(UART_DEV, O_RDONLY | O_NOCTTY);
    char uart_buf[100];

    while (1)
    {
        memset(uart_buf, 0, sizeof(uart_buf));
        int len = read(uart_fd, uart_buf, sizeof(uart_buf) - 1);

        if (len > 0)
        {
            uart_buf[len] = '\0';

            last_uart_time = time(NULL);
            uart_initialized = 1;

            if (strchr(uart_buf, '<') && strchr(uart_buf, '>'))
            {
                int t, h, a;

                if (sscanf(uart_buf, "<temp:%d,hum:%d,air:%d>", &t, &h, &a) == 3)
                {
                    if (t > 0 && t < 100 && h >= 0 && h <= 100 && a > 0 && a < 1000)
                    {
                        pthread_mutex_lock(&lock);
                        temp = t;
                        hum = h;
                        air = a;
                        data_ready = 1;
                        pthread_mutex_unlock(&lock);
                    }
                }
            }
        }
    }
}

// ===== MQTT Topic 定義 =====
#define TOPIC_STATUS       "integration/smart/v1/env/" DEVICE_ID "/status"
#define TOPIC_EVENT        "integration/smart/v1/env/" DEVICE_ID "/event"
#define TOPIC_AVAILABILITY "integration/smart/v1/env/" DEVICE_ID "/availability"

// ===== 發布 availability =====
static void publish_availability(struct mosquitto *mosq, int online)
{
    char msg[256];
    time_t now = time(NULL);
    snprintf(msg, sizeof(msg),
        "{"
        "\"type\":\"availability\","
        "\"node_id\":\"%s\","
        "\"ts\":%ld,"
        "\"availability\":{"
            "\"online\":%s"
        "}"
        "}",
        DEVICE_ID, (long)now,
        online ? "true" : "false");

    mosquitto_publish(mosq, NULL, TOPIC_AVAILABILITY,
        strlen(msg), msg, 1, true);
    printf("[availability] %s\n", msg);
}

// ===== MQTT disconnect callback =====
static void on_disconnect(struct mosquitto *mosq, void *obj, int rc)
{
    if (rc != 0)
    {
        printf("[MQTT] 異常斷線 (rc=%d)，嘗試重連...\n", rc);
        while (mosquitto_reconnect(mosq) != MOSQ_ERR_SUCCESS)
        {
            printf("[MQTT] 重連失敗，5 秒後重試...\n");
            sleep(5);
        }
        printf("[MQTT] 重連成功\n");
        publish_availability(mosq, 1);
    }
}

// ===== MQTT thread =====
void *mqtt_thread(void *arg)
{
    struct mosquitto *mosq;

    mosquitto_lib_init();
    mosq = mosquitto_new(NULL, true, NULL);

    if (!mosq)
    {
        printf("MQTT init failed\n");
        return NULL;
    }

    // 設定 disconnect callback
    mosquitto_disconnect_callback_set(mosq, on_disconnect);

    // 設定 LWT（broker 在異常斷線時自動發布 online:false）
    char lwt_msg[256];
    snprintf(lwt_msg, sizeof(lwt_msg),
        "{"
        "\"type\":\"availability\","
        "\"node_id\":\"%s\","
        "\"ts\":0,"
        "\"availability\":{"
            "\"online\":false"
        "}"
        "}",
        DEVICE_ID);
    mosquitto_will_set(mosq, TOPIC_AVAILABILITY,
        strlen(lwt_msg), lwt_msg, 1, true);

    mosquitto_connect(mosq, "localhost", 1883, 60);

    // 連線成功後主動發布 online:true
    publish_availability(mosq, 1);

    int last_occupancy = -1;  // 用來偵測 occupancy 變化，觸發 event
    int last_online    = -1;  // 用來偵測 UART availability 變化

    while (1)
    {
        mosquitto_loop(mosq, 0, 1);

        if (!uart_initialized)
        {
            sleep(1);
            continue;
        }

        pthread_mutex_lock(&lock);
        int m = motion;
        int t = temp;
        int h = hum;
        int a = air;
        pthread_mutex_unlock(&lock);

        time_t now = time(NULL);
        int disconnected = (now - last_uart_time > 5);
        int online = !disconnected;

        // ===== 發布 availability topic（UART 狀態改變時發布）=====
        if (online != last_online)
        {
            publish_availability(mosq, online);
            last_online = online;
        }

        // ===== 發布 status topic =====
        char status_msg[400];

        if (disconnected)
        {
            // UART 斷線：sensor 欄位用 null，occupancy 仍回報
            snprintf(status_msg, sizeof(status_msg),
                "{"
                "\"type\":\"status\","
                "\"node_id\":\"%s\","
                "\"ts\":%ld,"
                "\"status\":{"
                    "\"temperature_c\":null,"
                    "\"humidity_percent\":null,"
                    "\"air_raw\":null,"
                    "\"occupancy\":%s"
                "}"
                "}",
                DEVICE_ID, (long)now,
                m ? "true" : "false");
        }
        else
        {
            snprintf(status_msg, sizeof(status_msg),
                "{"
                "\"type\":\"status\","
                "\"node_id\":\"%s\","
                "\"ts\":%ld,"
                "\"status\":{"
                    "\"temperature_c\":%d,"
                    "\"humidity_percent\":%d,"
                    "\"air_raw\":%d,"
                    "\"occupancy\":%s"
                "}"
                "}",
                DEVICE_ID, (long)now,
                t, h, a,
                m ? "true" : "false");
        }

        mosquitto_publish(mosq, NULL, TOPIC_STATUS, strlen(status_msg), status_msg, 0, false);
        printf("[status] %s\n", status_msg);

        // ===== 偵測 occupancy 變化，發布 event topic =====
        if (m != last_occupancy && last_occupancy != -1)
        {
            char event_msg[300];
            snprintf(event_msg, sizeof(event_msg),
                "{"
                "\"type\":\"event\","
                "\"node_id\":\"%s\","
                "\"ts\":%ld,"
                "\"event\":{"
                    "\"category\":\"occupancy\","
                    "\"action\":\"changed\","
                    "\"value\":%s"
                "}"
                "}",
                DEVICE_ID, (long)now,
                m ? "true" : "false");

            mosquitto_publish(mosq, NULL, TOPIC_EVENT, strlen(event_msg), event_msg, 0, false);
            printf("[event] %s\n", event_msg);
        }
        last_occupancy = m;

        sleep(2);
    }
}

// ===== main =====
int main()
{
    pthread_t t1, t2, t3, t4;

    pthread_mutex_init(&lock, NULL);

    pthread_create(&t1, NULL, pir_thread, NULL);
    pthread_create(&t2, NULL, uart_thread, NULL);
    pthread_create(&t3, NULL, mqtt_thread, NULL);
    pthread_create(&t4, NULL, lcd_thread,  NULL);

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    pthread_join(t3, NULL);
    pthread_join(t4, NULL);

    return 0;
}

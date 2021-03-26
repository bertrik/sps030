#include <stdio.h>
#include <stdint.h>

#include <Arduino.h>
#include <EEPROM.h>
#include "lmic.h"
#include <hal/hal.h>
#include "arduino_lmic_hal_boards.h"

#include <SPI.h>
#include <SSD1306.h>
#include <SparkFunBME280.h>
#include "soc/efuse_reg.h"
#include "HardwareSerial.h"

// OTA
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>

#include "shdlc.h"
#include "sps30.h"
#include "editline.h"
#include "cmdproc.h"

// This EUI must be in BIG-ENDIAN format, most-significant byte (MSB).
// For TTN issued EUIs the first bytes should be 0x70, 0xB3, 0xD5.
static const uint8_t APPEUI[8] = { 0x70, 0xB3, 0xD5, 0x7E, 0xD0, 0x01, 0xA0, 0x9B };

// This key should be in big endian format as well, see above.
static const uint8_t APPKEY[] = {
    0xAA, 0x9F, 0x12, 0x45, 0x7F, 0x06, 0x64, 0xDF, 0x4C, 0x1E, 0x9F, 0xC9, 0x5E, 0xDA, 0x1A, 0x8A
};

#define printf Serial.printf

#define OLED_I2C_ADDR 0x3C

#define PIN_OLED_RESET  16
#define PIN_OLED_SDA    4
#define PIN_OLED_SCL    15
#define PIN_BUTTON      0
#define PIN_SDS_RX      22
#define PIN_SDS_TX      23
#define PIN_VEXT        21

#define UG_PER_M3       "\u00B5g/m\u00B3"

// total measurement cycle time (seconds)
#define TIME_CYCLE      300
// time to show version info
#define TIME_VERSION    5
// duration of warmup (seconds)
#define TIME_WARMUP     20
// duration of measurement (seconds)
#define TIME_MEASURE    10
// reboot interval (seconds)
#define REBOOT_INTERVAL 2592000UL
// time to keep display on (seconds)
#define TIME_OLED_ENABLED   600

// how we know the non-volatile storage contains meaningful data
#define NVDATA_MAGIC    "magic"

// structure of non-volatile data
typedef struct {
    uint8_t deveui[8];
    uint8_t appeui[8];
    uint8_t appkey[16];
    char magic[8];
} nvdata_t;

typedef struct {
    float humidity;
    float temperature;
    float pressure;
} bme_meas_t;

typedef struct {
    bool enabled;
    bool update;
    // 1st line: LoRa address
    char loraDevEui[32];
    // 2nd line: LoRa status
    char loraStatus[32];
    // 3rd line: PM10
    String dust1;
    // 4th line: PM2.5
    String dust2;
} screen_t;

// main state machine
typedef enum {
    E_INIT = 0,
    E_IDLE,
    E_WARMUP,
    E_MEASURE,
    E_SEND,
    E_LAST
} fsm_state_t;

// Pin mapping
const lmic_pinmap lmic_pins = *Arduino_LMIC::GetPinmap_ThisBoard();

// each measurement cycle takes 5 minutes, this table specifies how many cycles there are per transmission
static const int interval_table[] = {
    1,  // SF6
    1,  // SF7
    1,  // SF8
    2,  // SF9
    4,  // SF10
    8,  // SF11
    16  // SF12
};

//static fsm_state_t main_state;

static SSD1306 display(OLED_I2C_ADDR, PIN_OLED_SDA, PIN_OLED_SCL);
static HardwareSerial spsSerial(1);
static screen_t screen;
static unsigned long screen_last_enabled = 0;
static nvdata_t nvdata;
static char cmdline[200];
static rps_t last_tx_rps = 0;
static SPS30 sps(&spsSerial);

void os_getDevEui(u1_t * buf)
{
    for (int i = 0; i < 8; i++) {
        buf[i] = nvdata.deveui[7 - i];
    }
}

void os_getArtEui(u1_t * buf)
{
    for (int i = 0; i < 8; i++) {
        buf[i] = nvdata.appeui[7 - i];
    }
}

void os_getDevKey(u1_t * buf)
{
    memcpy(buf, nvdata.appkey, 16);
}

// saves OTAA keys to EEPROM
static void otaa_save(const uint8_t deveui[8], const uint8_t appeui[8], const uint8_t appkey[16])
{
    memcpy(&nvdata.deveui, deveui, 8);
    memcpy(&nvdata.appeui, appeui, 8);
    memcpy(&nvdata.appkey, appkey, 16);
    strcpy(nvdata.magic, NVDATA_MAGIC);
    EEPROM.put(0, nvdata);
    EEPROM.commit();
}

// restores OTAA parameters from EEPROM, returns false if there are none
static bool otaa_restore(void)
{
    EEPROM.get(0, nvdata);
    return strcmp(nvdata.magic, NVDATA_MAGIC) == 0;
}

// sets OTAA to defaults
static void otaa_defaults(void)
{
    uint64_t chipid = ESP.getEfuseMac();

    // setup of unique ids
    uint8_t deveui[8];
    deveui[0] = (chipid >> 56) & 0xFF;
    deveui[1] = (chipid >> 48) & 0xFF;
    deveui[2] = (chipid >> 40) & 0xFF;
    deveui[3] = (chipid >> 32) & 0xFF;
    deveui[4] = (chipid >> 24) & 0xFF;
    deveui[5] = (chipid >> 16) & 0xFF;
    deveui[6] = (chipid >> 8) & 0xFF;
    deveui[7] = (chipid >> 0) & 0xFF;
    otaa_save(deveui, APPEUI, APPKEY);
}

static void setLoraStatus(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(screen.loraStatus, sizeof(screen.loraStatus), fmt, args);
    va_end(args);

    screen.update = true;
}

const char *event_names[] = { LMIC_EVENT_NAME_TABLE__INIT };

static void onEventCallback(void *user, ev_t ev)
{
    Serial.print(os_getTime());
    Serial.print(": ");
    Serial.println(event_names[ev]);

    switch (ev) {
    case EV_JOINING:
        setLoraStatus("OTAA JOIN...");
        break;
    case EV_JOINED:
        setLoraStatus("JOIN OK!");
        break;
    case EV_JOIN_FAILED:
        setLoraStatus("JOIN failed!");
        break;
    case EV_REJOIN_FAILED:
        setLoraStatus("REJOIN failed!");
        break;
    case EV_TXCOMPLETE:
        if (LMIC.txrxFlags & TXRX_ACK)
            Serial.println("Received ack");
        if (LMIC.dataLen) {
            Serial.print("Received ");
            Serial.print(LMIC.dataLen);
            Serial.println(" bytes of payload");
        }
        setLoraStatus("%08X-%d", LMIC.devaddr, LMIC.seqnoUp);
        break;
    case EV_TXSTART:
        setLoraStatus("Transmit SF%d", getSf(LMIC.rps) + 6);
        last_tx_rps = LMIC.rps;
        break;
    case EV_RXSTART:
        setLoraStatus("Receive SF%d", getSf(LMIC.rps) + 6);
        break;
    case EV_JOIN_TXCOMPLETE:
        setLoraStatus("JOIN sent");
        break;
    default:
        Serial.print("Unknown event: ");
        Serial.println((unsigned) ev);
        break;
    }
}

static void screen_update(unsigned long int second)
{
    if (screen.update) {
        display.clear();

        // 1st line
        display.setFont(ArialMT_Plain_10);
        display.drawString(0, 0, screen.loraDevEui);

        // 2nd line
        display.setFont(ArialMT_Plain_16);
        display.drawString(0, 12, screen.loraStatus);

        // 3rd
        display.drawString(0, 30, screen.dust1);

        // 4th line
        display.drawString(0, 46, screen.dust2);

        display.display();
        screen.update = false;
    }
    if (screen.enabled && ((second - screen_last_enabled) > TIME_OLED_ENABLED)) {
        display.displayOff();
        screen.enabled = false;
    }
}

static int show_help(const cmd_t * cmds)
{
    for (const cmd_t * cmd = cmds; cmd->cmd != NULL; cmd++) {
        printf("%10s: %s\n", cmd->name, cmd->help);
    }
    return CMD_OK;
}

static int do_help(int argc, char *argv[]);

static int do_reboot(int argc, char *argv[])
{
    ESP.restart();
    return CMD_OK;
}

static void printhex(const uint8_t * buf, int len)
{
    for (int i = 0; i < len; i++) {
        printf("%02X", buf[i]);
    }
    printf("\n");
}

static void parsehex(const char *hex, uint8_t *buf, int len)
{
    char tmp[4];
    for (int i = 0; i < len; i++) {
        strncpy(tmp, hex, 2);
        *buf++ = strtoul(tmp, NULL, 16);
        hex += 2;
    }
}

static int do_otaa(int argc, char *argv[])
{
    // reset OTAA
    if ((argc == 2) && (strcmp(argv[1], "reset") == 0)) {
        printf("Resetting OTAA to defaults\n");
        otaa_defaults();
    }

    // save OTAA parameters
    if (argc == 4) {
        printf("Setting OTAA parameters\n");
        char *deveui_hex = argv[1];
        char *appeui_hex = argv[2];
        char *appkey_hex = argv[3];
        if ((strlen(deveui_hex) != 16) || (strlen(appeui_hex) != 16) || (strlen(appkey_hex) != 32)) {
            return CMD_ARG;
        }

        uint8_t deveui[8];
        uint8_t appeui[8];
        uint8_t appkey[16];
        parsehex(deveui_hex, deveui, 8);
        parsehex(appeui_hex, appeui, 8);
        parsehex(appkey_hex, appkey, 16);
        otaa_save(deveui, appeui, appkey);
    }

    // show current OTAA parameters
    printf("Dev EUI = ");
    printhex(nvdata.deveui, 8);
    printf("App EUI = ");
    printhex(nvdata.appeui, 8);
    printf("App key = ");
    printhex(nvdata.appkey, 16);

    return CMD_OK;
}

static int do_send(int argc, char *argv[])
{
    uint8_t buf[256];

    if (argc < 2) {
        return -1;
    }
    
    char *hex = argv[1];
    int len = strlen(hex) / 2;
    parsehex(hex, buf, len);
    
    printf("Sending %d bytes ...\n", len);
    spsSerial.write(buf, len);

    return 0;
}

static int sps_start(int argc, char *argv[])
{
    bool result = sps.start(false);
    return result ? CMD_OK : -1;
}

static int sps_stop(int argc, char *argv[])
{
    bool result = sps.stop();
    return result ? CMD_OK : -1;
}

static int sps_read_measurement(int argc, char *argv[])
{
    uint16_t pm1_0;
    uint16_t pm2_5;
    uint16_t pm4_0;
    uint16_t pm10;
    uint16_t ps;

    if (sps.read_measurement(&pm1_0, &pm2_5, &pm4_0, &pm10, &ps)) {
        printf("PM1.0 = %u\n", pm1_0);
        printf("PM2.5 = %u\n", pm2_5);
        printf("PM4.0 = %u\n", pm4_0);
        printf("PM10  = %u\n", pm10);
        printf("size  = %u\n", ps);
        return CMD_OK;
    }

    return -1;
}

static int sps_device_info(int argc, char *argv[])
{
    char product_type[20];
    char serial_nr[20];

    if (sps.device_info(product_type, serial_nr)) {
        printf("product type: %s\n", product_type);
        printf("serial_nr   : %s\n", serial_nr);
        return CMD_OK;
    }
    
    return -1;
}

static const cmd_t sps_commands[] = {
    {"00", sps_start, "Start measurement"},
    {"01", sps_stop, "Stop measurement"},
    {"03", sps_read_measurement, "Read measured value"},
    {"d0", sps_device_info, "Device information"},
    { NULL, NULL, NULL }
};

static int do_sps(int argc, char *argv[])
{
    if (argc < 2) {
        return show_help(sps_commands);
    }
    const cmd_t *cmd = cmd_find(sps_commands, argv[1]);
    if (cmd == NULL) {
        printf("Unhandled '%s', available commands:\n", argv[1]);
        return show_help(sps_commands);
    }
    return cmd->cmd(argc - 1, argv + 1);    
}

const cmd_t commands[] = {
    { "help", do_help, "Show help" },
    { "reboot", do_reboot, "Reboot ESP" },
    { "send", do_send, "<data>" },
    { "otaa", do_otaa, "[reset|<[deveui] [appeui] [appkey]>] Query/reset/set OTAA parameters" },
    { "sps", do_sps, "Execute SPS command" },
    { NULL, NULL, NULL }
};

static int do_help(int argc, char *argv[])
{
    return show_help(commands);
}

void setup(void)
{
    Serial.begin(115200);
    Serial.println("Starting...");

    EditInit(cmdline, sizeof(cmdline));

    // VEXT config: 0 = enable Vext
    pinMode(PIN_VEXT, OUTPUT);
    digitalWrite(PIN_VEXT, 0);

    // LED config
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, 0);

    // button config
    pinMode(PIN_BUTTON, INPUT_PULLUP);

    // init the OLED
    pinMode(PIN_OLED_RESET, OUTPUT);
    digitalWrite(PIN_OLED_RESET, LOW);
    delay(50);
    digitalWrite(PIN_OLED_RESET, HIGH);

    display.init();
    display.flipScreenVertically();
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    screen.enabled = true;

    // initialize the SPS30 serial
    pinMode(PIN_SDS_RX, INPUT);
    pinMode(PIN_SDS_TX, OUTPUT);
    spsSerial.begin(115200, SERIAL_8N1, PIN_SDS_RX, PIN_SDS_TX, false);

    // restore LoRaWAN keys from EEPROM, or use a default
    EEPROM.begin(sizeof(nvdata));
    if (!otaa_restore()) {
        otaa_defaults();
    }
    snprintf(screen.loraDevEui, sizeof(screen.loraDevEui),
             "%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X", nvdata.deveui[0], nvdata.deveui[1], nvdata.deveui[2],
             nvdata.deveui[3], nvdata.deveui[4], nvdata.deveui[5], nvdata.deveui[6], nvdata.deveui[7]);

    // LMIC init
    os_init();
    LMIC_reset();
    LMIC_registerEventCb(onEventCallback, NULL);
//   LMIC_startJoining();

    // fan on, this makes the SDS011 respond to version commands
//    sds_fan(true);

    // OTA init
    uint64_t chipid = ESP.getEfuseMac();
    char ssid[32];
    sprintf(ssid, "ESP32-%08X%08X", (uint32_t)(chipid >> 32), (uint32_t)chipid);
    WiFi.softAP(ssid);
    ArduinoOTA.setHostname("esp32-pmsensor");
    ArduinoOTA.begin();
}

void loop(void)
{
    unsigned long ms = millis();
    unsigned long second = ms / 1000UL;

    // parse command line
    if (Serial.available()) {
        char c;
        bool haveLine = EditLine(Serial.read(), &c);
        Serial.write(c);
        if (haveLine) {
            int result = cmd_process(commands, cmdline);
            switch (result) {
            case CMD_OK:
                printf("OK\n");
                break;
            case CMD_NO_CMD:
                break;
            case CMD_ARG:
                printf("Invalid arguments\n");
                break;
            case CMD_UNKNOWN:
                printf("Unknown command, available commands:\n");
                show_help(commands);
                break;
            default:
                printf("%d\n", result);
                break;
            }
            printf(">");
        }
    }

    // button press re-enabled the display
    if (digitalRead(PIN_BUTTON) == 0) {
        if (!screen.enabled) {
            display.displayOn();
            screen_last_enabled = second;
            screen.enabled = true;
        }
    }

    // update screen
    screen_update(second);

    // run LoRa process
    os_runloop_once();

    // reboot every 30 days
    if (second > REBOOT_INTERVAL) {
        printf("Reboot ...\n");
        ESP.restart();
        while (true);
    }

    // run the OTA process
    ArduinoOTA.handle();
}


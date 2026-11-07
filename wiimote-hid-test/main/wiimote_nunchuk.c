#include "wiimote_nunchuk.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "wiimote_protocol.h"

/*
 * Emulated Nunchuk on the extension port (I2C 0x52), register layout per
 * WiiBrew: data at 0x00, calibration at 0x20 (mirrored at 0x30), encryption
 * key at 0x40, encryption control at 0xf0 and the ID at 0xfa.
 * Encryption (0xaa written to 0xf0) is supported for the all-zero key only,
 * where WiiBrew documents every table entry as 0x97.
 */

static const char *TAG = "nunchuk";

#define NC_REG_DATA 0x00
#define NC_REG_CALIBRATION 0x20
#define NC_REG_CALIBRATION_MIRROR 0x30
#define NC_REG_KEY 0x40
#define NC_KEY_LEN 16
#define NC_REG_ENCRYPTION 0xf0
#define NC_ENCRYPTION_ENABLED 0xaa
#define NC_REG_ID 0xfa
#define NC_CALIBRATION_LEN 16
#define NC_ZERO_KEY_TABLE 0x97
#define NC_CALIBRATION_MAGIC 0x55

static uint8_t s_regs[0x100];
static bool s_attached;
static bool s_unsupported_key;
static uint32_t s_encryption_enables;
static wiimote_nunchuk_input_t s_input;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

void wiimote_nunchuk_neutral_input(wiimote_nunchuk_input_t *input)
{
    input->stick_x = WIIMOTE_NUNCHUK_STICK_CENTER;
    input->stick_y = WIIMOTE_NUNCHUK_STICK_CENTER;
    /* Held level: gravity on +Z. */
    input->accel[0] = WIIMOTE_NUNCHUK_ACCEL_ZERO;
    input->accel[1] = WIIMOTE_NUNCHUK_ACCEL_ZERO;
    input->accel[2] = WIIMOTE_NUNCHUK_ACCEL_ONE_G;
    input->c = false;
    input->z = false;
}

static void build_calibration(uint8_t calibration[NC_CALIBRATION_LEN])
{
    const uint8_t zero = (uint8_t)(WIIMOTE_NUNCHUK_ACCEL_ZERO >> 2);
    const uint8_t one_g = (uint8_t)(WIIMOTE_NUNCHUK_ACCEL_ONE_G >> 2);
    const uint8_t values[NC_CALIBRATION_LEN - 2] = {
        zero, zero, zero, 0x00,
        one_g, one_g, one_g, 0x00,
        0xe0, 0x20, WIIMOTE_NUNCHUK_STICK_CENTER,
        0xe0, 0x20, WIIMOTE_NUNCHUK_STICK_CENTER,
    };
    uint8_t checksum = NC_CALIBRATION_MAGIC;
    for (size_t i = 0; i < sizeof(values); ++i) checksum = (uint8_t)(checksum + values[i]);
    memcpy(calibration, values, sizeof(values));
    calibration[NC_CALIBRATION_LEN - 2] = checksum;
    calibration[NC_CALIBRATION_LEN - 1] = (uint8_t)(checksum + NC_CALIBRATION_MAGIC);
}

static void reset_registers_locked(void)
{
    static const uint8_t id[6] = {0x00, 0x00, 0xa4, 0x20, 0x00, 0x00};
    uint8_t calibration[NC_CALIBRATION_LEN];
    build_calibration(calibration);
    memset(s_regs, 0, sizeof(s_regs));
    memcpy(&s_regs[NC_REG_CALIBRATION], calibration, sizeof(calibration));
    memcpy(&s_regs[NC_REG_CALIBRATION_MIRROR], calibration, sizeof(calibration));
    memcpy(&s_regs[NC_REG_ID], id, sizeof(id));
    s_unsupported_key = false;
}

static void ensure_initialized_locked(void)
{
    static bool initialized;
    if (initialized) return;
    initialized = true;
    reset_registers_locked();
    wiimote_nunchuk_neutral_input(&s_input);
}

void wiimote_nunchuk_reset(void)
{
    portENTER_CRITICAL(&s_mux);
    ensure_initialized_locked();
    reset_registers_locked();
    portEXIT_CRITICAL(&s_mux);
}

bool wiimote_nunchuk_set_attached(bool attached)
{
    portENTER_CRITICAL(&s_mux);
    ensure_initialized_locked();
    const bool changed = s_attached != attached;
    s_attached = attached;
    /* A freshly plugged extension starts uninitialized and unencrypted. */
    if (changed && attached) reset_registers_locked();
    portEXIT_CRITICAL(&s_mux);
    if (changed) ESP_LOGI(TAG, "Nunchuk %s", attached ? "attached" : "detached");
    return changed;
}

bool wiimote_nunchuk_is_attached(void)
{
    portENTER_CRITICAL(&s_mux);
    const bool attached = s_attached;
    portEXIT_CRITICAL(&s_mux);
    return attached;
}

void wiimote_nunchuk_set_input(const wiimote_nunchuk_input_t *input)
{
    if (input == NULL) return;
    portENTER_CRITICAL(&s_mux);
    ensure_initialized_locked();
    s_input = *input;
    portEXIT_CRITICAL(&s_mux);
}

static bool encrypted_locked(void)
{
    return s_regs[NC_REG_ENCRYPTION] == NC_ENCRYPTION_ENABLED;
}

static bool zero_key_locked(void)
{
    for (size_t i = 0; i < NC_KEY_LEN; ++i) {
        if (s_regs[NC_REG_KEY + i] != 0) return false;
    }
    return true;
}

void wiimote_nunchuk_get_status(wiimote_nunchuk_status_t *status)
{
    portENTER_CRITICAL(&s_mux);
    ensure_initialized_locked();
    status->attached = s_attached;
    status->encrypted = encrypted_locked();
    status->unsupported_key = s_unsupported_key;
    status->encryption_enables = s_encryption_enables;
    status->input = s_input;
    portEXIT_CRITICAL(&s_mux);
}

static void update_data_locked(void)
{
    const wiimote_nunchuk_input_t *in = &s_input;
    uint8_t *data = &s_regs[NC_REG_DATA];
    data[0] = in->stick_x;
    data[1] = in->stick_y;
    data[2] = (uint8_t)(in->accel[0] >> 2);
    data[3] = (uint8_t)(in->accel[1] >> 2);
    data[4] = (uint8_t)(in->accel[2] >> 2);
    data[5] = (uint8_t)(((in->accel[2] & 3U) << 6) | ((in->accel[1] & 3U) << 4) |
                        ((in->accel[0] & 3U) << 2) |
                        (in->c ? 0U : 0x02U) | (in->z ? 0U : 0x01U));
}

/* Copies registers as the host sees them, applying zero-key encryption if enabled. */
static void copy_out_locked(uint16_t address, uint8_t length, uint8_t *output)
{
    update_data_locked();
    memcpy(output, &s_regs[address], length);
    if (!encrypted_locked()) return;
    if (!zero_key_locked()) {
        s_unsupported_key = true;
        return;
    }
    for (uint8_t i = 0; i < length; ++i) {
        output[i] = (uint8_t)((uint8_t)(output[i] - NC_ZERO_KEY_TABLE) ^ NC_ZERO_KEY_TABLE);
    }
}

bool wiimote_nunchuk_passthrough_info(uint8_t id[6], uint8_t calibration[16])
{
    portENTER_CRITICAL(&s_mux);
    ensure_initialized_locked();
    const bool attached = s_attached;
    if (attached) {
        memcpy(id, &s_regs[NC_REG_ID], 6);
        memcpy(calibration, &s_regs[NC_REG_CALIBRATION], NC_CALIBRATION_LEN);
    }
    portEXIT_CRITICAL(&s_mux);
    return attached;
}

bool wiimote_nunchuk_read(uint8_t slave, uint16_t address, uint8_t length,
                          uint8_t *output, uint8_t *error)
{
    if (slave != WIIMOTE_I2C_SLAVE_EXTENSION) return false;
    address &= 0xff; /* WiiBrew: the middle address byte is ignored (0xA4xxyy). */
    portENTER_CRITICAL(&s_mux);
    ensure_initialized_locked();
    if (!s_attached) {
        portEXIT_CRITICAL(&s_mux);
        return false;
    }
    if ((uint32_t)address + length > sizeof(s_regs)) {
        portEXIT_CRITICAL(&s_mux);
        *error = WIIMOTE_MEMORY_ERROR_INVALID_ADDRESS;
        return true;
    }
    copy_out_locked(address, length, output);
    portEXIT_CRITICAL(&s_mux);
    *error = WIIMOTE_MEMORY_ERROR_SUCCESS;
    return true;
}

bool wiimote_nunchuk_write(uint8_t slave, uint16_t address, const uint8_t *data,
                           uint8_t count, uint8_t *error)
{
    if (slave != WIIMOTE_I2C_SLAVE_EXTENSION) return false;
    address &= 0xff; /* WiiBrew: the middle address byte is ignored (0xA4xxyy). */
    portENTER_CRITICAL(&s_mux);
    ensure_initialized_locked();
    if (!s_attached) {
        portEXIT_CRITICAL(&s_mux);
        return false;
    }
    if ((uint32_t)address + count > sizeof(s_regs)) {
        portEXIT_CRITICAL(&s_mux);
        *error = WIIMOTE_MEMORY_ERROR_INVALID_ADDRESS;
        return true;
    }
    memcpy(&s_regs[address], data, count);
    const bool enabled = address <= NC_REG_ENCRYPTION &&
                         address + count > NC_REG_ENCRYPTION && encrypted_locked();
    if (enabled) ++s_encryption_enables;
    const bool key_written = address < NC_REG_KEY + NC_KEY_LEN &&
                             address + count > NC_REG_KEY;
    const bool zero_key = zero_key_locked();
    if (key_written) s_unsupported_key = false;
    portEXIT_CRITICAL(&s_mux);

    if (enabled) ESP_LOGW(TAG, "Host enabled extension encryption (0xaa -> 0xf0)");
    if (key_written)
        ESP_LOGW(TAG, "Host wrote encryption key bytes at 0x%02x (%u); key %s",
                 address, count, zero_key ? "all zero (supported)" : "non-zero (unsupported)");
    *error = WIIMOTE_MEMORY_ERROR_SUCCESS;
    return true;
}

void wiimote_nunchuk_encode(uint8_t *output, uint8_t length)
{
    portENTER_CRITICAL(&s_mux);
    ensure_initialized_locked();
    copy_out_locked(NC_REG_DATA, length, output);
    portEXIT_CRITICAL(&s_mux);
}

void wiimote_nunchuk_encode_passthrough(uint8_t output[WIIMOTE_NUNCHUK_DATA_LEN])
{
    portENTER_CRITICAL(&s_mux);
    ensure_initialized_locked();
    update_data_locked();
    uint8_t d[WIIMOTE_NUNCHUK_DATA_LEN];
    memcpy(d, &s_regs[NC_REG_DATA], sizeof(d));
    portEXIT_CRITICAL(&s_mux);

    output[0] = d[0];
    output[1] = d[1];
    output[2] = d[2];
    output[3] = d[3];
    output[4] = (uint8_t)((d[4] & 0xfeU) | 0x01U);
    output[5] = (uint8_t)(((d[4] & 0x01U) << 7) | /* AZ<2> */
                          ((d[5] & 0x80U) >> 1) | /* AZ<1> */
                          (d[5] & 0x20U) |        /* AY<1> */
                          ((d[5] & 0x08U) << 1) | /* AX<1> */
                          ((d[5] & 0x02U) << 2) | /* C */
                          ((d[5] & 0x01U) << 2)); /* Z */
}

#include <stdint.h>

/* =========================================================
   REFLEX ARENA - STM32F103C8T6 BLUE PILL
   Direct registers only; no HAL, CMSIS header, or .ioc
   CPU clock assumed: 8 MHz

   LEDs:    PB0, PB1, PB10
   Buttons: PA0, PA1, PA2 (active LOW)
   Start:   PA3 (active LOW)
   OLED:    SSD1306 128x64, address 0x3C
   SDA: PB6, SCL: PB7
   ========================================================= */

/* Hardware registers */
#define RCC_APB2ENR (*(volatile uint32_t *)0x40021018U)

#define GPIOA_CRL   (*(volatile uint32_t *)0x40010800U)
#define GPIOA_IDR   (*(volatile uint32_t *)0x40010808U)
#define GPIOA_BSRR  (*(volatile uint32_t *)0x40010810U)

#define GPIOB_CRL   (*(volatile uint32_t *)0x40010C00U)
#define GPIOB_CRH   (*(volatile uint32_t *)0x40010C04U)
#define GPIOB_BSRR  (*(volatile uint32_t *)0x40010C10U)
#define GPIOB_BRR   (*(volatile uint32_t *)0x40010C14U)

#define SYST_CSR    (*(volatile uint32_t *)0xE000E010U)
#define SYST_RVR    (*(volatile uint32_t *)0xE000E014U)
#define SYST_CVR    (*(volatile uint32_t *)0xE000E018U)

/* Pins */
#define LED1       (1U << 0)
#define LED2       (1U << 1)
#define LED3       (1U << 10)

#define BTN1       (1U << 0)
#define BTN2       (1U << 1)
#define BTN3       (1U << 2)
#define START_BTN  (1U << 3)

#define SCL_PIN    (1U << 7)
#define SDA_PIN    (1U << 6)

#define OLED_ADDR      0x3CU
#define GAME_LENGTH_MS 20000U
#define DEBOUNCE_MS    8U

/* Time and game state shared with SysTick */
static volatile uint32_t g_ms = 0U;
static volatile uint8_t game_active = 0U;
static volatile uint32_t game_start_ms = 0U;
static volatile uint32_t game_score = 0U;
static volatile uint8_t game_target = 0U;
static volatile uint32_t game_second = 0U;

static uint32_t high_score = 0U;
static uint32_t random_state = 0xA53C91E7U;

/* Random target history */
static uint8_t previous1 = 0U;
static uint8_t previous2 = 0U;

/* Button debounce state, maintained by SysTick */
static uint8_t candidate_buttons = 0U;
static uint8_t stable_buttons = 0U;
static uint32_t candidate_since = 0U;

/* Function declarations used by SysTick */
static void leds_off(void);
static void set_led_pattern(uint8_t mask);
static uint8_t button_mask(void);
static uint8_t random_pattern(void);
static uint8_t bit_count(uint8_t value);

/* ---------------- Timebase and game interrupt ---------------- */

/*
 * Runs every 1 ms.
 *
 * While the game is active:
 * - Changes target exactly on each second boundary.
 * - Samples and debounces buttons independently of OLED work.
 * - Awards one point per newly pressed matching button.
 */
void SysTick_Handler(void)
{
    uint32_t now = ++g_ms;

    if (game_active == 0U) {
        return;
    }

    uint32_t elapsed = now - game_start_ms;

    /* End the game after 20,000 ms. */
    if (elapsed >= GAME_LENGTH_MS) {
        game_active = 0U;
        leds_off();
        return;
    }

    /* Change target on each one-second boundary. */
    uint32_t current_second = elapsed / 1000U;

    if (current_second != game_second) {
        game_second = current_second;
        game_target = random_pattern();
        set_led_pattern(game_target);
    }

    /* Sample button inputs every millisecond. */
    uint8_t raw = button_mask();

    if (raw != candidate_buttons) {
        candidate_buttons = raw;
        candidate_since = now;
    }

    /* Accept a changed input only after 8 ms stable. */
    if ((candidate_buttons != stable_buttons) &&
        ((uint32_t)(now - candidate_since) >= DEBOUNCE_MS)) {

        uint8_t newly_pressed =
            (uint8_t)(candidate_buttons &
                      (uint8_t)(~stable_buttons));

        stable_buttons = candidate_buttons;

        /* Only newly pressed buttons matching the target score. */
        uint8_t correct =
            (uint8_t)(newly_pressed & game_target);

        if (correct != 0U) {
            game_score += bit_count(correct);
        }
    }
}

static uint32_t millis(void)
{
    return g_ms;
}

static void delay_ms(uint32_t ms)
{
    uint32_t start = millis();

    while ((uint32_t)(millis() - start) < ms) {
        /* SysTick continues running during this wait. */
    }
}

static void delay_short(volatile uint32_t count)
{
    while (count--) {
        __asm volatile ("nop");
    }
}

/* ---------------- LEDs ---------------- */

static void leds_off(void)
{
    GPIOB_BRR = LED1 | LED2 | LED3;
}

/* Bit 0=PB0, bit 1=PB1, bit 2=PB10 */
static void set_led_pattern(uint8_t mask)
{
    uint32_t pins = 0U;

    if ((mask & 1U) != 0U) pins |= LED1;
    if ((mask & 2U) != 0U) pins |= LED2;
    if ((mask & 4U) != 0U) pins |= LED3;

    GPIOB_BRR = LED1 | LED2 | LED3;
    GPIOB_BSRR = pins;
}

/* ---------------- Buttons ---------------- */

static uint8_t button_mask(void)
{
    uint32_t pins = GPIOA_IDR;
    uint8_t mask = 0U;

    if ((pins & BTN1) == 0U) mask |= 1U;
    if ((pins & BTN2) == 0U) mask |= 2U;
    if ((pins & BTN3) == 0U) mask |= 4U;

    return mask;
}

/* ---------------- Random targets ---------------- */

static uint32_t random_next(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;

    if (random_state == 0U) {
        random_state = 0xA53C91E7U;
    }

    return random_state;
}

/*
 * Select a nonzero pattern from 1 to 7.
 * Reject any candidate that would cause an LED to appear
 * in three consecutive target seconds.
 *
 * The history is updated only after a valid candidate is found.
 */
static uint8_t random_pattern(void)
{
    uint8_t candidate;

    do {
        candidate = (uint8_t)(1U + (random_next() % 7U));
    } while ((candidate & previous1 & previous2) != 0U);

    previous2 = previous1;
    previous1 = candidate;

    return candidate;
}

static uint8_t bit_count(uint8_t value)
{
    uint8_t count = 0U;

    while (value != 0U) {
        count += (uint8_t)(value & 1U);
        value >>= 1;
    }

    return count;
}

/* ---------------- Software I2C ---------------- */

static void scl_release(void) { GPIOB_BSRR = SCL_PIN; }
static void scl_low(void)     { GPIOB_BRR = SCL_PIN; }
static void sda_release(void) { GPIOB_BSRR = SDA_PIN; }
static void sda_low(void)     { GPIOB_BRR = SDA_PIN; }

static void i2c_start(void)
{
    sda_release();
    scl_release();
    delay_short(80U);

    sda_low();
    delay_short(80U);

    scl_low();
    delay_short(80U);
}

static void i2c_stop(void)
{
    sda_low();
    delay_short(80U);

    scl_release();
    delay_short(80U);

    sda_release();
    delay_short(80U);
}

static void i2c_write_byte(uint8_t value)
{
    for (uint8_t i = 0U; i < 8U; i++) {
        if ((value & 0x80U) != 0U) {
            sda_release();
        } else {
            sda_low();
        }

        delay_short(50U);
        scl_release();
        delay_short(80U);
        scl_low();
        delay_short(50U);

        value <<= 1;
    }

    /* Ninth clock for ACK; ACK is not sampled. */
    sda_release();
    delay_short(50U);

    scl_release();
    delay_short(80U);

    scl_low();
    delay_short(50U);
}

static void oled_command(uint8_t command)
{
    i2c_start();
    i2c_write_byte((uint8_t)(OLED_ADDR << 1));
    i2c_write_byte(0x00U);
    i2c_write_byte(command);
    i2c_stop();
}

static void oled_data_begin(void)
{
    i2c_start();
    i2c_write_byte((uint8_t)(OLED_ADDR << 1));
    i2c_write_byte(0x40U);
}

static void oled_data_end(void)
{
    i2c_stop();
}

/* ---------------- OLED driver ---------------- */

static void oled_init(void)
{
    static const uint8_t commands[] = {
        0xAE,
        0xD5, 0x80,
        0xA8, 0x3F,
        0xD3, 0x00,
        0x40,
        0x8D, 0x14,
        0x20, 0x02,
        0xA1,
        0xC8,
        0xDA, 0x12,
        0x81, 0x7F,
        0xD9, 0xF1,
        0xDB, 0x40,
        0xA4,
        0xA6,
        0xAF
    };

    for (uint32_t i = 0U; i < sizeof(commands); i++) {
        oled_command(commands[i]);
    }
}

static void oled_clear(void)
{
    for (uint8_t page = 0U; page < 8U; page++) {
        oled_command((uint8_t)(0xB0U | page));
        oled_command(0x00U);
        oled_command(0x10U);

        oled_data_begin();

        for (uint16_t col = 0U; col < 128U; col++) {
            i2c_write_byte(0x00U);
        }

        oled_data_end();
    }
}

/* Font: uppercase letters, digits, and exclamation mark */
static const uint8_t font[37][5] = {
    {0x7E,0x11,0x11,0x11,0x7E}, /* A */
    {0x7F,0x49,0x49,0x49,0x36}, /* B */
    {0x3E,0x41,0x41,0x41,0x22}, /* C */
    {0x7F,0x41,0x41,0x22,0x1C}, /* D */
    {0x7F,0x49,0x49,0x49,0x41}, /* E */
    {0x7F,0x09,0x09,0x09,0x01}, /* F */
    {0x3E,0x41,0x49,0x49,0x7A}, /* G */
    {0x7F,0x08,0x08,0x08,0x7F}, /* H */
    {0x00,0x41,0x7F,0x41,0x00}, /* I */
    {0x20,0x40,0x41,0x3F,0x01}, /* J */
    {0x7F,0x08,0x14,0x22,0x41}, /* K */
    {0x7F,0x40,0x40,0x40,0x40}, /* L */
    {0x7F,0x02,0x0C,0x02,0x7F}, /* M */
    {0x7F,0x02,0x04,0x08,0x7F}, /* N */
    {0x3E,0x41,0x41,0x41,0x3E}, /* O */
    {0x7F,0x09,0x09,0x09,0x06}, /* P */
    {0x3E,0x41,0x51,0x21,0x5E}, /* Q */
    {0x7F,0x09,0x19,0x29,0x46}, /* R */
    {0x46,0x49,0x49,0x49,0x31}, /* S */
    {0x01,0x01,0x7F,0x01,0x01}, /* T */
    {0x3F,0x40,0x40,0x40,0x3F}, /* U */
    {0x1F,0x20,0x40,0x20,0x1F}, /* V */
    {0x3F,0x40,0x38,0x40,0x3F}, /* W */
    {0x63,0x14,0x08,0x14,0x63}, /* X */
    {0x07,0x08,0x70,0x08,0x07}, /* Y */
    {0x61,0x51,0x49,0x45,0x43}, /* Z */

    {0x3E,0x51,0x49,0x45,0x3E}, /* 0 */
    {0x00,0x42,0x7F,0x40,0x00}, /* 1 */
    {0x42,0x61,0x51,0x49,0x46}, /* 2 */
    {0x21,0x41,0x45,0x4B,0x31}, /* 3 */
    {0x18,0x14,0x12,0x7F,0x10}, /* 4 */
    {0x27,0x45,0x45,0x45,0x39}, /* 5 */
    {0x3C,0x4A,0x49,0x49,0x30}, /* 6 */
    {0x01,0x71,0x09,0x05,0x03}, /* 7 */
    {0x36,0x49,0x49,0x49,0x36}, /* 8 */
    {0x06,0x49,0x49,0x29,0x1E}, /* 9 */

    {0x00,0x00,0x5F,0x00,0x00}  /* ! */
};

static void get_glyph(char c, uint8_t out[5])
{
    int index = -1;

    if (c >= 'A' && c <= 'Z') {
        index = c - 'A';
    } else if (c >= '0' && c <= '9') {
        index = 26 + c - '0';
    } else if (c == '!') {
        index = 36;
    }

    for (uint8_t i = 0U; i < 5U; i++) {
        out[i] = (index >= 0) ? font[index][i] : 0x00U;
    }
}

static void oled_print_line(uint8_t page, const char *text)
{
    oled_command((uint8_t)(0xB0U | page));
    oled_command(0x00U);
    oled_command(0x10U);

    oled_data_begin();

    uint8_t count = 0U;

    while (*text && count < 21U) {
        uint8_t glyph[5];
        get_glyph(*text++, glyph);

        for (uint8_t i = 0U; i < 5U; i++) {
            i2c_write_byte(glyph[i]);
        }

        i2c_write_byte(0x00U);
        count++;
    }

    while (count < 21U) {
        for (uint8_t i = 0U; i < 6U; i++) {
            i2c_write_byte(0x00U);
        }

        count++;
    }

    oled_data_end();
}

static void screen4(const char *a,
                    const char *b,
                    const char *c,
                    const char *d)
{
    oled_clear();
    oled_print_line(0U, a);
    oled_print_line(2U, b);
    oled_print_line(4U, c);
    oled_print_line(6U, d);
}

static void screen4_update(const char *a,
                           const char *b,
                           const char *c,
                           const char *d)
{
    oled_print_line(0U, a);
    oled_print_line(2U, b);
    oled_print_line(4U, c);
    oled_print_line(6U, d);
}

/* ---------------- Start screen ---------------- */

static void wait_start_release(void)
{
    while ((GPIOA_IDR & START_BTN) == 0U) {
        delay_ms(1U);
    }

    delay_ms(25U);
}

static void wait_for_start(void)
{
    leds_off();

    screen4("REFLEX ARENA",
            "READY!",
            "PRESS START",
            "20 SECOND GAME");

    while (1) {
        if ((GPIOA_IDR & START_BTN) == 0U) {
            delay_ms(25U);

            if ((GPIOA_IDR & START_BTN) == 0U) {
                wait_start_release();
                return;
            }
        }
    }
}

static void countdown_sequence(void)
{
    leds_off();

    screen4("READY", "", "", "");
    delay_ms(700U);

    screen4("SET", "", "", "");
    delay_ms(700U);

    screen4("GO!", "", "", "");
    delay_ms(300U);
}

/* ---------------- Game ---------------- */

static uint32_t run_game(void)
{
    countdown_sequence();

    /* Mix the random sequence between games. */
    random_state ^= millis() ^ GPIOA_IDR ^ 0x9E3779B9U;

    if (random_state == 0U) {
        random_state = 0xA53C91E7U;
    }

    /* Reset per-game state before enabling the game. */
    previous1 = 0U;
    previous2 = 0U;

    candidate_buttons = button_mask();
    stable_buttons = candidate_buttons;
    candidate_since = millis();

    game_score = 0U;
    game_second = 0U;
    game_start_ms = millis();

    game_target = random_pattern();
    set_led_pattern(game_target);

    /* Enable interrupt-driven target timing and scoring. */
    game_active = 1U;

    uint32_t last_display_second = 0xFFFFFFFFU;
    uint32_t last_display_score = 0xFFFFFFFFU;

    while (game_active != 0U) {
        uint32_t elapsed = millis() - game_start_ms;

        uint32_t remaining =
            (elapsed >= GAME_LENGTH_MS)
            ? 0U
            : ((GAME_LENGTH_MS - elapsed + 999U) / 1000U);

        uint32_t shown_score = game_score;
        uint32_t shown_second = elapsed / 1000U;

        /*
         * Display updates may take time, but SysTick continues
         * target timing and button sampling during I2C writes.
         */
        if ((shown_second != last_display_second) ||
            (shown_score != last_display_score)) {

            char time_line[] = "TIME 20";
            char score_line[] = "SCORE 0000";

            if (remaining > 20U) {
                remaining = 20U;
            }

            time_line[5] =
                (char)('0' + ((remaining / 10U) % 10U));

            time_line[6] =
                (char)('0' + (remaining % 10U));

            if (shown_score > 9999U) {
                shown_score = 9999U;
            }

            score_line[6] =
                (char)('0' + ((shown_score / 1000U) % 10U));

            score_line[7] =
                (char)('0' + ((shown_score / 100U) % 10U));

            score_line[8] =
                (char)('0' + ((shown_score / 10U) % 10U));

            score_line[9] =
                (char)('0' + (shown_score % 10U));

            screen4_update("REFLEX ARENA",
                           time_line,
                           score_line,
                           "PRESS LIT BUTTON");

            last_display_second = shown_second;
            last_display_score = game_score;
        }
    }

    leds_off();

    /* Read the final score after the game has stopped. */
    return game_score;
}

/* ---------------- End-of-game flashes ---------------- */

static void end_flash(void)
{
    for (uint8_t i = 0U; i < 3U; i++) {
        set_led_pattern(7U);
        delay_ms(150U);

        leds_off();
        delay_ms(150U);
    }
}

/* ---------------- Results screen ---------------- */

static void show_results(uint32_t score)
{
    if (score > high_score) {
        high_score = score;
    }

    char score_line[] = "SCORE 0000";
    char high_line[] = "HIGH  0000";

    uint32_t s = score;
    uint32_t h = high_score;

    if (s > 9999U) s = 9999U;
    if (h > 9999U) h = 9999U;

    score_line[6] =
        (char)('0' + ((s / 1000U) % 10U));
    score_line[7] =
        (char)('0' + ((s / 100U) % 10U));
    score_line[8] =
        (char)('0' + ((s / 10U) % 10U));
    score_line[9] =
        (char)('0' + (s % 10U));

    high_line[6] =
        (char)('0' + ((h / 1000U) % 10U));
    high_line[7] =
        (char)('0' + ((h / 100U) % 10U));
    high_line[8] =
        (char)('0' + ((h / 10U) % 10U));
    high_line[9] =
        (char)('0' + (h % 10U));

    screen4("GAME OVER",
            score_line,
            high_line,
            "READY IN 5 SEC");

    delay_ms(5000U);
}

/* ---------------- GPIO initialization ---------------- */

static void gpio_init(void)
{
    /* Enable GPIOA and GPIOB clocks. */
    RCC_APB2ENR |= (1U << 2) | (1U << 3);

    /* PA0-PA3 input pull-up. */
    GPIOA_CRL &= ~0xFFFFU;
    GPIOA_CRL |= 0x8888U;
    GPIOA_BSRR = BTN1 | BTN2 | BTN3 | START_BTN;

    /* PB0 and PB1 push-pull outputs, 2 MHz. */
    GPIOB_CRL &= ~((0xFU << 0) | (0xFU << 4));
    GPIOB_CRL |=  ((0x2U << 0) | (0x2U << 4));

    /* PB6 and PB7 open-drain software I2C. */
    GPIOB_CRL &= ~((0xFU << 24) | (0xFU << 28));
    GPIOB_CRL |=  ((0x6U << 24) | (0x6U << 28));

    /* PB10 push-pull output, 2 MHz. */
    GPIOB_CRH &= ~(0xFU << 8);
    GPIOB_CRH |=  (0x2U << 8);

    leds_off();
    sda_release();
    scl_release();
}

/* Assumes an 8 MHz CPU clock and SysTick clock = CPU clock. */
static void systick_init(void)
{
    SYST_CSR = 0U;
    SYST_RVR = 7999U;
    SYST_CVR = 0U;
    SYST_CSR = 0x07U;
}

/* ---------------- Entry point ---------------- */

int main(void)
{
    gpio_init();
    systick_init();

    delay_ms(100U);

    oled_init();
    oled_clear();

    while (1) {
        wait_for_start();

        uint32_t score = run_game();

        leds_off();
        end_flash();
        show_results(score);

        /* Next loop returns to the READY screen. */
    }
}

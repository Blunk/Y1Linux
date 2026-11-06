/* SPDX-License-Identifier: GPL-2.0-only
 * Innioasis Y1 twenty-third-boot additions (docs/planning/Y1-CHARGE-MODE-PLAN.md, stage C1), included after
 * y1-tenth-boot.h only when Y1_TWENTY_THIRD_BOOT is defined, so the earlier PID1 binaries stay byte-identical.
 * A charge-only mode before the Y2 system, as the Y2's offline-charge: with a charger and the battery under
 * 3.6 V nothing else starts (no Reborn, no power daemon, whose voltage guard would shut a flat player down and,
 * with USB attached, restart it into the same state); the backlight is low, the voltage and charge state are on
 * screen, the USB gadget is the kernel's, so a PC still grants 450 mA. A 2 s hold of the centre button starts the
 * system once three fresh samples read at least 3.4 V. Without a charger: under 3.4 V power off, otherwise start.
 * Unplugging during the charge mode powers off. The centre button held through the boot (still down when PID1
 * gets here) enters the mode on a charger at any voltage. Only reads and the backlight; no PMIC write of its own.
 */
enum { CHARGE_MODE_UV = 3600000, START_UV = 3400000, START_SAMPLES = 3, HOLD_TICKS = 20, UNPLUG_SAMPLES = 3,
       KEY_CENTRE = 28, CHARGE_BACKLIGHT = 4 };
#define Y1_BATTERY "/sys/class/power_supply/BAT0"
#ifdef Y1_TWENTY_NINTH_BOOT
static int charge_screen_open(const char *light);
static void charge_screen_tick(u32 now, int pressed, int held, int ready);
static void charge_screen_close(void);
static void chg_light_on(int on);
#endif
#define EVIOCGKEY_96 0x80604518 /* _IOR('E', 0x18, 96 bytes): the keys held now */

/* 1 when the centre button is held right now. A button already down before the key driver probed sends no press,
 * so the PMIC's own level comes first: CHRSTATUS 0x142 bit 1 PWRKEY_DEB, clear while pressed (poweroff-policy.h). */
static int centre_held(void)
{
    unsigned chrstatus;
    long pmic = regmap_file[0] ? call5(SYS_OPEN, (long)regmap_file, O_RDONLY, 0, 0, 0) : -1;
    int read = pmic >= 0 && pmic_read(pmic, 0x142, &chrstatus);
    if (pmic >= 0) call5(SYS_CLOSE, pmic, 0, 0, 0, 0);
    if (read && !(chrstatus & 0x02)) return 1;
    char node[] = "/dev/input/event0";
    for (unsigned i = 0; i < INPUT_NODES; ++i) {
        u8 state[96];
        node[16] = (char)('0' + i);
        long fd = call5(SYS_OPEN, (long)node, O_RDONLY | O_NONBLOCK, 0, 0, 0);
        if (fd < 0) continue;
        long r = call5(SYS_IOCTL, fd, EVIOCGKEY_96, (long)state, 0, 0);
        call5(SYS_CLOSE, fd, 0, 0, 0, 0);
        if (r >= 0 && (state[KEY_CENTRE / 8] >> (KEY_CENTRE % 8) & 1)) return 1;
    }
    return 0;
}

/* The battery's voltage (uV) and whether a charger is present; 0 when the charger driver has no sample yet. */
static int battery_sample(int *uv, int *charger)
{
    char text[32];
    *uv = 0;
    if (read_text(Y1_BATTERY "/voltage_now", text, sizeof(text)) <= 0 || !parse_number(text, uv) || *uv <= 0)
        return 0;
    if (read_text(Y1_BATTERY "/status", text, sizeof(text)) <= 0) return 0;
    *charger = !same(text, "Discharging");
    return 1;
}

static void put_volts(int uv)
{
    char digits[4];
    int mv = uv / 1000;
    put_number(mv / 1000);
    put(".");
    digits[0] = (char)('0' + mv / 100 % 10);
    digits[1] = (char)('0' + mv / 10 % 10);
    digits[2] = (char)('0' + mv % 10);
    digits[3] = 0;
    put(digits);
    put(" V");
}

static void set_brightness(const char *value)
{
    char path[80];
    unsigned n = 0;
    while (value[n]) ++n;
    copy(path, sizeof(path), backlight_dir, "/brightness", 0);
    long fd = call5(SYS_OPEN, (long)path, O_WRONLY, 0, 0, 0);
    if (fd >= 0) { call5(SYS_WRITE, fd, (long)value, n, 0, 0); call5(SYS_CLOSE, fd, 0, 0, 0, 0); }
}

static void charge_power_off(const char *why)
{
#ifdef Y1_TWENTY_NINTH_BOOT
    chg_light_on(0); /* the charging screen, if shown, goes dark first */
#endif
#ifdef Y1_TWENTY_EIGHTH_BOOT
    console_to("/dev/tty0");
#endif
    put("charge: "); put(why); put(", powering off\n"); send(SCREEN | USB);
    sleep_seconds(3);
    call5(SYS_REBOOT, (long)0xfee1dead, 0x28121969, 0x4321fedc, 0, 0); /* LINUX_REBOOT_CMD_POWER_OFF */
}

/* Returns when the Y2 system may start; powers off otherwise. */
static void charge_gate(void)
{
    int uv = 0, charger = 0, have = 0;
    for (unsigned tenth = 0; tenth < 150 && !(have = battery_sample(&uv, &charger)); ++tenth) sleep_tenths(1);
    if (!have) { say("charge: no battery sample, starting the system\n"); return; }
    if (!charger) {
        if (uv < START_UV) charge_power_off("battery low and no charger");
        return;
    }
    if (uv >= CHARGE_MODE_UV && !centre_held()) return;

    char brightness[8] = "32";
    char path[80];
    copy(path, sizeof(path), backlight_dir, "/brightness", 0);
    long len = read_text(path, brightness, sizeof(brightness));
    if (len <= 0) copy(brightness, sizeof(brightness), "32", 0, 0);
#ifdef Y1_TWENTY_NINTH_BOOT
    int graphics = charge_screen_open(brightness); /* the Y2's charging screen; text only without it */
#else
    int graphics = 0;
#endif
    if (!graphics) {
        set_brightness("4");
#ifdef Y1_TWENTY_EIGHTH_BOOT
        console_to("/dev/tty0"); /* the charge-only mode talks on the screen */
#endif
    }
    put("charge: battery "); put_volts(uv); put(" on a charger: charge-only mode; hold the centre button 2 s to start\n");
    send(SCREEN | USB);

    long keys[INPUT_NODES];
    char node[] = "/dev/input/event0";
    for (unsigned i = 0; i < INPUT_NODES; ++i) {
        node[16] = (char)('0' + i);
        keys[i] = call5(SYS_OPEN, (long)node, O_RDONLY | O_NONBLOCK, 0, 0, 0);
    }
    unsigned good = 0, unplugged = 0, held_since = 0, holding = 0;
#ifdef Y1_TWENTY_NINTH_BOOT
    charge_screen_tick(0, 1, 0, 0);
#endif
    for (unsigned tick = 1;; ++tick) {
        int start = 0, pressed = 0;
        for (unsigned i = 0; i < INPUT_NODES; ++i) {
            struct input_event32 e;
            while (keys[i] >= 0 && call5(SYS_READ, keys[i], (long)&e, sizeof(e), 0, 0) == (long)sizeof(e)) {
                if (e.type != 1 || e.code != KEY_CENTRE) continue;
                pressed = 1;
                if (e.value == 1) { holding = 1; held_since = tick; }
                else if (e.value == 0) {
                    if (holding && tick - held_since >= HOLD_TICKS) start = 1;
                    holding = 0;
                }
            }
        }
        if (tick % TICKS_PER_SECOND == 0 && battery_sample(&uv, &charger)) {
            good = uv >= START_UV ? good + 1 : 0;
            unplugged = charger ? 0 : unplugged + 1;
            if (unplugged >= UNPLUG_SAMPLES) charge_power_off("charger removed");
            if (tick % (TICKS_PER_SECOND * 10) == 0) {
                put("charge: "); put_volts(uv); put(good >= START_SAMPLES ? ", ready to start\n" : "\n");
                send(SCREEN);
            }
        }
        if (start) {
            if (good >= START_SAMPLES) break;
            put("charge: "); put_volts(uv); put(" is too low to start (3.400 V needed)\n"); send(SCREEN);
        }
#ifdef Y1_TWENTY_NINTH_BOOT
        charge_screen_tick(tick * 100, pressed, holding && tick - held_since >= HOLD_TICKS, good >= START_SAMPLES);
#else
        (void)pressed;
#endif
        sleep_tenths(1);
    }
    for (unsigned i = 0; i < INPUT_NODES; ++i) if (keys[i] >= 0) call5(SYS_CLOSE, keys[i], 0, 0, 0, 0);
#ifdef Y1_TWENTY_NINTH_BOOT
    charge_screen_close();
#endif
    (void)graphics;
    set_brightness(brightness);
#ifdef Y1_TWENTY_EIGHTH_BOOT
    console_to("/dev/ttyS0");
#endif
    put("charge: "); put_volts(uv); put(", starting the system\n"); send(SCREEN | USB);
}

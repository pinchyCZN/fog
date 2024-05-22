#include "SPI.h"
#include "ILI9341_t3n.h"
#include <XPT2046_Touchscreen.h>
// *************** Change to your Pin numbers ***************
#define TFT_DC  9
#define TFT_CS 10
#define TFT_RST 19
#define TFT_SCK 13
#define TFT_MISO 12
#define TFT_MOSI 11
#define TOUCH_CS  8
#define TRIGGER_PIN 2
#define T_CLK 13
#define T_CS 8
#define T_DIN 11
#define T_DO 12
#define T_IRQ 3

/*
    LI9341 Pin  Teensy 4.x     Notes
    VCC         VIN            Power: 3.6 to 5.5 volts
    GND         GND
    CS          10             Alternate Pins: 9, 15, 20, 21
    RESET       19
    D/C         9              Alternate Pins: 10, 15, 20, 21
    SDI (MOSI)  11 (DOUT)
    SCK         13 (SCK)
    LED         VIN            Use 100 ohm resistor
    SDO (MISO)  12 (DIN)
    T_CLK       13 (SCK)
    T_CS        8              Alternate: any digital pin
    T_DIN       11 (DOUT)
    T_DO        12 (DIN)
    T_IRQ       3              Optional: can use any digital pin
*/

ILI9341_t3n tft = ILI9341_t3n(TFT_CS, TFT_DC, TFT_RST, TFT_MOSI, TFT_SCK, TFT_MISO);
//ILI9341_t3n tft = ILI9341_t3n(TFT_CS, TFT_DC, TFT_RST);
XPT2046_Touchscreen ts(T_CS, T_IRQ);
DMAMEM uint16_t fb1[320 * 240];
DMAMEM uint16_t fb2[320 * 240];
int current_fb = 0;

void setup() {
    tft.begin();
    tft.setTextSize(2);
    tft.setRotation(0);
    tft.fillScreen(ILI9341_BLACK);
    ts.begin();
    Serial1.begin(460800);
    Serial1.setTimeout(10);
    pinMode(TRIGGER_PIN, OUTPUT);
}

char last_error[20] = {0};
long last_time = 0;

void print_error(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(last_error, sizeof(last_error), fmt, args);
    last_time = millis();
}

struct FOG_DATA {
    long rotation;
    long temp;
    double d_rot;
    double d_temp;
    double total;
    double history[100];
    unsigned long ticks[100];
    int index;
    unsigned long tick;
    double total_tmp;
    double total_delta;
    unsigned long total_tick;
    double total_tmp2;
    double total_delta2;
    unsigned long total_tick2;
};

FOG_DATA fog_data = {0};

void send_pulse()
{
    static unsigned long last = 0;
    unsigned long tick = millis();
    unsigned long delta = tick - last;
    if (delta < 2) {
        delay(1);
    }
    last = millis();
    digitalWrite(TRIGGER_PIN, HIGH);
    delayMicroseconds(100);
    digitalWrite(TRIGGER_PIN, LOW);
}

int generate_fake_data(byte *data, int len)
{
    long val = random(0, 0x1f0000);
    long temp = random(0, 10) * 16;
    int i;
    int index = 0;
    byte crc = 0;
    if (len < 10) {
        return 0;
    }
    data[index++] = 0x80;
    for (i = 0; i < 5; i++) {
        byte a = (val >> (7 * i)) & 0x7F;
        data[index++] = a;
        crc ^= a;
    }
    data[index++] = crc;
    crc = 0;
    for (i = 0; i < 2; i++) {
        byte a = (temp >> (7 * i)) & 0x7F;
        data[index++] = a;
        crc ^= a;
    }
    data[index++] = crc;
    return index;
}

int read_serial(byte *data, int len)
{
    int amount = 0;
    //amount = generate_fake_data(data, len);
    if (0 == amount) {
        amount = Serial1.readBytes((char*)data, len);
    }
    return amount;
}
void drain_data()
{
    int amount = Serial1.available();
    if (amount > 0) {
        char tmp[64];
        if (amount > 64) {
            amount = 64;
        }
        print_error("drain data");
        Serial1.readBytes(tmp, amount);
    }
}

void read_serial_data()
{
    byte data[12] = {0};
    double d = 0;
    // max 1 degree before overflow
    const double factor = 134476195.0;
    int i, amount;
    byte crc = 0;
    long rotation = 0;
    long temp = 0;
    send_pulse();
    amount = read_serial(data, 10);
    if (amount < 10) {
        print_error("not enuf data %i", amount);
        return;
    }
    if (data[0] != 0x80) {
        print_error("invalid header");
        return;
    }
    for (i = 0; i < 5; i++) {
        byte a = data[1 + i];
        rotation |= a << (7 * i);
        crc ^= a;
    }
    if (crc != data[6]) {
        print_error("invalid crc rotation");
        return;
    }
    crc = 0;
    for (i = 0; i < 2; i++) {
        byte a = data[7 + i];
        temp |= a << (7 * i);
        crc ^= a;
    }
    if (crc != data[9]) {
        print_error("invalid crc temp");
        return;
    }
    d = rotation;
    d = d / factor;
    fog_data.rotation = rotation;
    fog_data.temp = temp;
    fog_data.d_temp = temp / 16.0;
    fog_data.d_rot = d;
    fog_data.total += d;
    fog_data.tick = micros();

}

void read_fog_data()
{
    read_serial_data();
    static unsigned long last = 0;
    unsigned long current = millis();
    unsigned long delta = current - last;
    if (delta >= 50) {
        FOG_DATA *fg = &fog_data;
        fg->history[fg->index] = fg->total;
        fg->ticks[fg->index] = fg->tick;
        fg->index++;
        int count = sizeof(fg->history) / sizeof(fg->history[0]);
        if (fg->index >= count) {
            fg->index = 0;
        }
        last = current;
    }
    {
        FOG_DATA *fg = &fog_data;
        if (0 == fg->total_tick) {
            fg->total_tmp = fg->total;
            fg->total_tick = current;
        } else if ((current - fg->total_tick) >= 10000) {
            fg->total_tick = 0;
            fg->total_delta = fg->total - fg->total_tmp;
        }
        static unsigned long delay_start = 0;
        if (0 == delay_start) {
            delay_start = current;
        } else if ((current - delay_start) > 5000) {
            if (0 == fg->total_tick2) {
                fg->total_tmp2 = fg->total;
                fg->total_tick2 = current;
            } else {
                fg->total_delta2 = fg->total - fg->total_tmp2;
                if ((current - fg->total_tick2) > (3600 * 1000 * 48)) {
                    fg->total_tick2 = 0;
                }
            }
        }
    }
    drain_data();

}

unsigned long g_tick = 0;
unsigned long g_delta = 0;
void tick_start()
{
    g_tick = millis();
}
void tick_end()
{
    g_delta = millis() - g_tick;
}

void loop(void) {
    tick_start();
    read_fog_data();
    print_fog_data();
    tick_end();
}

double calculate_slope()
{
    double m, xbar, ybar;
    int count = sizeof(fog_data.history) / sizeof(fog_data.history[0]);
    int i;
    FOG_DATA *fg = &fog_data;
    xbar = ybar = 0;
    for (i = 0; i < count; i++) {
        ybar += fg->history[i];
        xbar += fg->ticks[i];
    }
    xbar /= count;
    ybar /= count;
    double top, bottom;
    top = bottom = 0;
    for (i = 0; i < count; i++) {
        double x, y, a, b;
        y = fg->history[i];
        x = fg->ticks[i];
        a = (x - xbar) * (y - ybar);
        b = (x - xbar) * (x - xbar);
        top += a;
        bottom += b;
    }
    m = 0;
    if (bottom != 0) {
        m = top / bottom;
    }
    return m;
}
void print_fog_data()
{
    static unsigned long tick = 0;
    unsigned long delta, current;
    current = millis();
    delta = current - tick;
    if (delta < 125) {
        return;
    }
    if (tft.asyncUpdateActive()) {
        return;
    }
    tick = current;
    tft.setFrameBuffer((current_fb & 1) ? fb2 : fb1);
    tft.useFrameBuffer(true);
    tft.fillScreen(ILI9341_BLACK);
    tft.setCursor(0, 0);
    if (1) {
        tft.print("  rot=");
        tft.println(fog_data.d_rot, 8);
        tft.print("total=");
        tft.println(fmod(fog_data.total, 360.0), 8);
        tft.print(" temp=");
        tft.println(fog_data.d_temp, 2);
        tft.print("\ntick=");
        tft.println(millis());
        tft.print("loop=");
        tft.println(g_delta);
        if (last_error[0]) {
            tft.println(last_error);
            tft.println(last_time);
        }
        else {
            tft.println("");
        }
        tft.print("d1/hr=");
        tft.print(fog_data.total_delta * (3600.0 / 10.0), 4);
        tft.print(" ");
        tft.println((millis() - fog_data.total_tick) / 1000);
        double tmp = (millis() - fog_data.total_tick2);
        tmp /= 1000.0;
        if ((tmp != 0) && fog_data.total_tick2) {
            tft.print("d2/hr=");
            tft.print(fog_data.total_delta2 * (3600.0 / tmp), 4);
            tft.print(" ");
            tft.println(round(tmp));
        }
        double a = calculate_slope() * 1000.0 * 1000.0 * 1000.0;
        tft.print("dv/hr=");
        tft.println(a * 3.6, 4);
        tft.print("\n");
        tft.print(fog_data.index);
        tft.print(" ");
        static int last_x = 0;
        if (ts.tirqTouched()) {
            TS_Point p = ts.getPoint();
            if (last_x != p.x) {
                fog_data.total_tick2 = 0;
                fog_data.total = 0;
                last_x = p.x;
            }
            tft.print(p.x);
            tft.print(" ");
            tft.print(p.y);
        } else {
            last_x = 0;
        }
        tft.println("");
    }
    plot_history();
    tft.updateScreenAsync();
    //tft.updateScreen();
    current_fb += 1;
}

void plot_history()
{
    FOG_DATA *fg = &fog_data;
    const int count = sizeof(fog_data.history) / sizeof(fog_data.history[0]);
    int i;
    const int y_offset = 250;
    tft.drawRect(0, y_offset, count * 2, 2, ILI9341_RED);
    for (i = 0; i < count - 1; i++) {
        double a = fg->history[i];
        double b = fg->history[i + 1];
        double tmp = b - a;
        const double scale = 200;
        const double scale2 = 2;
        const double inc = 10;
        if (tmp > 0) {
            tmp = log(tmp * scale);
            //if(i<12)
            //tft.println(tmp,10);
            tmp += inc;
            tmp *= scale2;
            if (tmp < 0)
                tmp = 0;
        } else if (tmp < 0) {
            tmp = log(-tmp * scale);
            //if(i<12)
            //tft.println(tmp,10);
            tmp += inc;
            tmp *= scale2;
            if (tmp < 0)
                tmp = 0;
            tmp = -tmp;
        }
        int y = tmp;
        tft.drawRect(i * 2, y + y_offset, 2, 2, ILI9341_GREEN);
    }
}

unsigned long testText() {
    tft.fillScreen(ILI9341_BLACK);
    unsigned long start = micros();
    tft.setCursor(0, 0);
    tft.setTextColor(ILI9341_WHITE);  tft.setTextSize(1);
    tft.println("Hello World!");
    tft.setTextColor(ILI9341_YELLOW); tft.setTextSize(2);
    tft.println(1234.56);
    tft.setTextColor(ILI9341_RED);    tft.setTextSize(3);
    tft.println(0xDEADBEEF, HEX);
    tft.println();
    tft.setTextColor(ILI9341_GREEN);
    tft.setTextSize(5);
    tft.println("Groop");
    tft.setTextSize(2);
    tft.println("I implore thee,");
    tft.setTextSize(1);
    tft.println("my foonting turlingdromes.");
    tft.println("And hooptiously drangle me");
    tft.println("with crinkly bindlewurdles,");
    tft.println("Or I will rend thee");
    tft.println("in the gobberwarts");
    tft.println("with my blurglecruncheon,");
    tft.println("see if I don't!");
    return micros() - start;
}

// ============================================================
//  Acelerómetro: MMA8452QR1 (I2C 0x1C, SA0=GND)
//  Pantalla:     OLED 0.96" SSD1306 (I2C 0x3C)
//  INT1 → D0 (A0) | INT2 → D1 (A1)
//  SDA  → D4      | SCL  → D5
//
//  Librerías necesarias :
//    - Adafruit SSD1306   (by Adafruit)
//    - Adafruit GFX       (by Adafruit)
// ============================================================

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ── OLED ────────────────────────────────────────────────────
#define SCREEN_W   128
#define SCREEN_H    64
#define OLED_ADDR  0x3C
Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);

// ── MMA8452 ─────────────────────────────────────────────────
#define MMA_ADDR       0x1C   // SA0 = GND → 0x1C  |  SA0 = VDD → 0x1D

// Registros principales
#define REG_STATUS     0x00
#define REG_OUT_X_MSB  0x01
#define REG_WHO_AM_I   0x0D   // debe devolver 0x2A
#define REG_XYZ_CFG    0x0E
#define REG_CTRL1      0x2A
#define REG_CTRL4      0x2D   // habilitar interrupciones
#define REG_CTRL5      0x2E   // rutear interrupciones

// Escala (±2g por defecto, resolución 12 bits con signo)
#define SCALE_G        2.0f
#define COUNTS_PER_G   2048   // 2^11

// ── Pines de interrupción ────────────────────────────────────
#define INT1_PIN  D0
#define INT2_PIN  D1

volatile bool newDataReady = false;

// ============================================================
//  Funciones I2C para MMA8452
// ============================================================
void mmaWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(MMA_ADDR);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

uint8_t mmaRead(uint8_t reg) {
  Wire.beginTransmission(MMA_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom(MMA_ADDR, (uint8_t)1);
  return Wire.available() ? Wire.read() : 0xFF;
}

// Lee los 6 bytes de salida (X, Y, Z) en una sola transacción
void mmaReadXYZ(int16_t &x, int16_t &y, int16_t &z) {
  Wire.beginTransmission(MMA_ADDR);
  Wire.write(REG_OUT_X_MSB);
  Wire.endTransmission(false);
  Wire.requestFrom(MMA_ADDR, (uint8_t)6);

  x = (int16_t)((Wire.read() << 8) | Wire.read()) >> 4;
  y = (int16_t)((Wire.read() << 8) | Wire.read()) >> 4;
  z = (int16_t)((Wire.read() << 8) | Wire.read()) >> 4;
}

// ============================================================
//  Inicialización del MMA8452
// ============================================================
bool mmaInit() {
  // Verificar que sea el chip correcto
  uint8_t who = mmaRead(REG_WHO_AM_I);
  if (who != 0x2A) {
    Serial.print("WHO_AM_I inesperado: 0x");
    Serial.println(who, HEX);
    return false;
  }

  // Poner en standby para configurar
  mmaWrite(REG_CTRL1, 0x00);
  delay(5);

  // Rango ±2g, sin filtro de paso alto
  mmaWrite(REG_XYZ_CFG, 0x00);

  // Habilitar interrupción de datos listos (DRDY) en INT1
  mmaWrite(REG_CTRL4, 0x01);  // INT_EN_DRDY
  mmaWrite(REG_CTRL5, 0x01);  // INT_CFG_DRDY → pin INT1

  // Modo activo | Low Noise | ODR 100 Hz
  //   CTRL1: ACTIVE=1, LNOISE=1, DR=010 (100Hz) → 0b00001101 = 0x0D
  mmaWrite(REG_CTRL1, 0x0D);

  return true;
}

// ============================================================
//  ISR — nueva muestra lista
// ============================================================
void onDataReady() {
  newDataReady = true;
}

// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("=== Hermes iniciando ===");

  Wire.begin();

  // ── Iniciar OLED ──
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("[ERROR] OLED no encontrada (0x3C)");
    while (true) delay(1000);
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(22, 24);
  display.println("Iniciando Hermes...");
  display.display();
  delay(800);

  // ── Iniciar MMA8452 ──
  if (!mmaInit()) {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("ERROR: MMA8452");
    display.println("no encontrado");
    display.println("");
    display.println("Verifica:");
    display.println(" SA0 -> GND");
    display.println(" Dir I2C: 0x1C");
    display.display();
    Serial.println("[ERROR] MMA8452 no encontrado en 0x1C");
    while (true) delay(1000);
  }

  // ── Configurar pin de interrupción ──
  pinMode(INT1_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(INT1_PIN), onDataReady, FALLING);

  Serial.println("[OK] MMA8452 listo — dirección 0x1C");
  Serial.println("[OK] OLED lista — dirección 0x3C");
  Serial.println("X(g)\t\tY(g)\t\tZ(g)");
}

// ============================================================
//  LOOP
// ============================================================
void loop() {
  // Solo leer cuando INT1 avisó que hay datos nuevos
  if (!newDataReady) return;
  newDataReady = false;

  // Leer ejes crudos
  int16_t rawX, rawY, rawZ;
  mmaReadXYZ(rawX, rawY, rawZ);

  // Convertir a g
  float gX = (float)rawX / COUNTS_PER_G * SCALE_G;
  float gY = (float)rawY / COUNTS_PER_G * SCALE_G;
  float gZ = (float)rawZ / COUNTS_PER_G * SCALE_G;

  // Calcular magnitud del vector (útil para detectar golpes)
  float mag = sqrt(gX * gX + gY * gY + gZ * gZ);

  // ── Serial (para debug en PC) ──
  Serial.printf("X: %+.3f\tY: %+.3f\tZ: %+.3f\t|a|: %.3f g\n",
                gX, gY, gZ, mag);

  // ── OLED ──────────────────────────────────────────────────
  display.clearDisplay();

  // Encabezado
  display.setTextSize(1);
  display.setCursor(28, 0);
  display.print("-- HERMES --");
  display.drawLine(0, 10, 127, 10, SSD1306_WHITE);

  // Valores X, Y, Z
  display.setTextSize(1);

  display.setCursor(0, 15);
  display.print("X:");
  display.setCursor(14, 15);
  display.printf("%+.3f g", gX);

  display.setCursor(0, 27);
  display.print("Y:");
  display.setCursor(14, 27);
  display.printf("%+.3f g", gY);

  display.setCursor(0, 39);
  display.print("Z:");
  display.setCursor(14, 39);
  display.printf("%+.3f g", gZ);

  // Línea separadora
  display.drawLine(0, 50, 127, 50, SSD1306_WHITE);

  // Magnitud en la parte inferior
  display.setCursor(0, 54);
  display.print("|a|:");
  display.setCursor(24, 54);
  display.printf("%.3f g", mag);

  // Indicador de movimiento brusco (>1.5g de magnitud)
  if (mag > 1.5f) {
    display.setCursor(80, 54);
    display.print("SHAKE!");
  }

  display.display();
}

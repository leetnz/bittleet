//
// Attitude Benchmark App
// Monitors and reports time spent doing attitude computations.
//
// Hoani Bryson (github.com/hoani)
// Copyright (c) 2021 Leetware Limited.
// License - MIT
//

#include "AttitudeBenchmark.h"
#include "../Attitude.h"

#include <I2Cdev.h>
#include <MPU6050.h>


static MPU6050 mpu;

static void initI2C() {
  Wire.begin();
  Wire.setClock(400000);
}

void AttitudeBenchmark::setup() {
  Serial.begin(115200);
  while (!Serial);

  initI2C();
  mpu.initialize();
  Serial.println(mpu.testConnection() ? "MPU6050 connection successful" : "MPU6050 connection failed");
}

void AttitudeBenchmark::loop() {
  static Attitude::Attitude attitude;
  uint32_t dt = micros();
  Attitude::GravityMeasurement g;
  mpu.getAcceleration(&g.x, &g.y, &g.z);
  dt = micros() - dt;

  Serial.print("dt (us): ");
  Serial.print(dt);
  Serial.print("\t");
  Serial.print("ax: ");
  Serial.print(g.x);
  Serial.print("\t");
  Serial.print("ay: ");
  Serial.print(g.y);
  Serial.print("\t");
  Serial.print("az: ");
  Serial.print(g.z);
  Serial.print("\n");
  delay(1000);
  dt = micros();
  Attitude::Angles angles = attitude.update(g);
  dt = micros() - dt;
  Serial.print("dt (us): ");
  Serial.print(dt);
  Serial.print("\t");
  Serial.print("roll: ");
  Serial.print(angles.roll);
  Serial.print("\t");
  Serial.print("pitch: ");
  Serial.print(angles.pitch);
  Serial.print("\n");

}
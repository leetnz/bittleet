/* Main Arduino sketch for OpenCat, the bionic quadruped walking robot.
   Updates should be posted on GitHub: https://github.com/PetoiCamp/OpenCat

   Rongzhong Li
   Jan.3rd, 2021
   Copyright (c) 2021 Petoi LLC.

   This sketch may also includes others' codes under MIT or other open source liscence.
   Check those liscences in corresponding module test folders.
   Feel free to contact us if you find any missing references.

   The MIT License

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:
  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.
  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.
*/
#define MAIN_SKETCH
#include "Bittleet.h"

#include "../OpenCat.h"
#include "../command/Command.h"

#include "../3rdParty/I2Cdev/I2Cdev.h"
#include "../3rdParty/MPU6050/MPU6050.h"


#include <Adafruit_NeoPixel.h>

#include "../state/Battery.h"

#include "../ui/Comms.h"
#include "../ui/Infrared.h"

#include "../state/Attitude.h"

#include "../skill/Skill.h"
#include "../skill/LoaderEeprom.h"

#include "../scheduler/Scheduler.h"

static MPU6050 mpu;

// NeoPixel integration
#define PIXEL_PIN 10
#define PIXEL_COUNT 7
Adafruit_NeoPixel pixels(PIXEL_PIN, PIXEL_COUNT, NEO_GRB + NEO_KHZ800);

#define BAUD_RATE 115200



#include "../3rdParty/MemoryFree/MemoryFree.h"

#include "../3rdParty/IRremote/src/IRremote.h"
IRrecv irrecv(IR_RECEIVER);     

// Local variables

//control related variables
static Command::Command lastCmd;
static int8_t offsetLR = 0;
static bool checkGyro = true;

static int8_t servoCalibs[DOF] = {};

static Skill::Skill skill;
static Skill::Loader* loader;

static Attitude::Attitude attitude{};


static void doPostureCommand(Command::Command& cmd, byte angleDataRatio = 1, float speedRatio = 1, bool shutServoAfterward = true) {
    loader->load(cmd, skill);
    if (skill.type != Skill::Type::Posture) {
        return;
    }
    transform(skill.spec, angleDataRatio, speedRatio);
    if (shutServoAfterward) {
        shutServos();
        cmd = Command::Command(Command::Simple::Rest);
    }
}

static void updateAttitude() {
    Attitude::Measurement m;
    m.us = micros();
    mpu.getMotion6(&m.accel.x, &m.accel.y, &m.accel.z, &m.gyro.x, &m.gyro.y, &m.gyro.z);
    m.accel.x = -m.accel.x;
    m.accel.y = -m.accel.y;
    m.gyro.x = -m.gyro.x;
    m.gyro.y = -m.gyro.y;
    attitude.update(m);
}

#define LARGE_PITCH_RAD (LARGE_PITCH * M_DEG2RAD)
#define LARGE_ROLL_RAD (LARGE_ROLL * M_DEG2RAD)

  


static void checkBodyMotion(Command::Command& newCmd)  {
    static uint8_t balanceRecover = 0;
    updateAttitude();
    bool recovering = false;

    if ((fabs(attitude.pitch()) > LARGE_PITCH_RAD  || fabs(attitude.roll()) > LARGE_ROLL_RAD )) {
        recovering = true;
        if (balanceRecover != 0) {
            if (fabs(attitude.roll()) > LARGE_ROLL_RAD) {
                newCmd = Command::Command(Command::Simple::Recover);
            }
        }
        balanceRecover = 10;
        attitude.reset();
    } else if (balanceRecover != 0) { // recover
        attitude.reset(); // Keep the attitude reset - we want the latest gravity attitudes.
        recovering = true;
        balanceRecover--;
        if (balanceRecover == 0) {
            // TODO: Investigate this - I don't know if we need to set newCmd == lastCmd
            //       - observe bittle recovery if we remove this line
            newCmd = lastCmd;
            lastCmd = Command::Command(Command::Simple::Balance);
            attitude.reset();
            updateAttitude();
            meow();
            recovering = false;
        }
    }

    if (recovering) {
        rollDeviation = 0.0;
        pitchDeviation = 0.0;
    } else {
        const float rollDev = attitude.roll() * M_RAD2DEG - skill.nominalRoll;
        const float pitchDev = attitude.pitch() * M_RAD2DEG - skill.nominalPitch;
    
        // IIR Hack to attempt to improve the compensation - TODO: actually use updateIIR in math/Filters.h
        rollDeviation = (8.0/16.0)*rollDeviation + (8.0/16.0)*rollDev;
        pitchDeviation = (8.0/16.0)*pitchDeviation + (8.0/16.0)*pitchDev;

        if (fabs(rollDeviation) < ROLL_LEVEL_TOLERANCE) {
            rollDeviation = 0.0;
        }
        if (fabs(pitchDeviation) < PITCH_LEVEL_TOLERANCE) {
            pitchDeviation = 0.0;
        }
    }
}

static void doBehaviorSkill(Skill::Skill& skill) {
    int8_t repeat = skill.loopSpec.count - 1;
    int8_t frameSize = 20;
    const int8_t angleMultiplier = (skill.doubleAngles) ? 2 : 1;
    for (uint8_t c = 0; c < skill.frames; c++) { //the last two in the row are transition speed and delay
        transform(skill.spec + c * frameSize, angleMultiplier, skill.spec[16 + c * frameSize] / 4.0);

        if (skill.spec[18 + c * frameSize]) {
            int triggerAxis = skill.spec[18 + c * frameSize];
            float triggerAngle = (float)skill.spec[19 + c * frameSize] * M_DEG2RAD;

            float currentAngle = attitude.angleFromAxis(triggerAxis);
            float previousAngle = currentAngle;
            while (1) {
                updateAttitude();
                currentAngle = attitude.angleFromAxis(triggerAxis);
                PT(currentAngle);
                PTF("\t");
                PTL(triggerAngle);
                if ((M_PI - fabs(currentAngle) > 2.0)  //skip the angle when the reading jumps from 180 to -180
                    && (triggerAxis * currentAngle < triggerAxis * triggerAngle && triggerAxis * previousAngle > triggerAxis * triggerAngle )) {
                    //the sign of triggerAxis will deterine whether the current angle should be larger or smaller than the trigger angle
                    break;
                }
                previousAngle = currentAngle;
            }
        } else {
            delay(skill.spec[17 + c * frameSize] * 50);
        }
        if (c == skill.loopSpec.finalRow && repeat > 0) {
            c = skill.loopSpec.firstRow - 1;
            repeat--;
        }
    }
}

#define NUM_TASKS (3)
#define INPUT_PERIOD_US (15000)
#define ATTITUDE_PERIOD_US (5000)
#define MOTION_PERIOD_US (20000)
static Scheduler::Scheduler<NUM_TASKS> scheduler{};
#define TASK_ATTITUDE (0)
#define TASK_INPUT (1)
#define TASK_MOTION (2)

static void initScheduler(){
    scheduler.registerTask(ATTITUDE_PERIOD_US);
    scheduler.registerTask(INPUT_PERIOD_US);
    scheduler.registerTask(MOTION_PERIOD_US);
}

static void initI2C() {
    Wire.begin();
    Wire.setClock(400000);
}

static void initIMU() {
    mpu.initialize();
    PTL(mpu.testConnection() ? F("MPU6050 connection successful") : F("MPU6050 connection failed"));

    delay(500);
    // supply your own gyro offsets here, scaled for min sensitivity
    for (byte i = 0; i < 4; i++) {
        PT(EEPROMReadInt(MPUCALIB + 4 + i * 2));
        PTF(" ");
    }
    mpu.setZAccelOffset(EEPROMReadInt(MPUCALIB + 4));
    mpu.setXGyroOffset(EEPROMReadInt(MPUCALIB + 6));
    mpu.setYGyroOffset(EEPROMReadInt(MPUCALIB + 8));
    mpu.setZGyroOffset(EEPROMReadInt(MPUCALIB + 10));

    mpu.setDLPFMode(2); // Effectively 100Hz bandwidth for gyro and accel
    mpu.setFullScaleAccelRange(MPU6050_ACCEL_FS_2); // Don't need anything beyond 2g
    mpu.setFullScaleGyroRange(MPU6050_GYRO_FS_1000);
}

static void processNewCommand(Command::Command& newCmd, Command::Move& move, bool& enableMotion, uint8_t& firstMotionJoint, uint8_t& frameIndex);
static void doMotionTask(bool enableMotion, const Skill::Skill& skill, uint8_t firstMotionJoint, uint8_t& frameIndex);
static void doMotionPosture(const Skill::Skill& skill);
static void doMotionMove(const Skill::Skill& skill, uint8_t firstMotionJoint, uint8_t& frameIndex);
static void doInputTask(Command::Move& move, bool& enableMotion, uint8_t& firstMotionJoint, uint8_t& frameIndex);
static void doAttitudeTask(Command::Move& move, bool& enableMotion, uint8_t& firstMotionJoint, uint8_t& frameIndex);

void Bittleet::setup() {
    skill = Skill::Skill::Empty();
    loader = new Skill::LoaderEeprom();
    pinMode(BUZZER, OUTPUT);

    initScheduler();

    Serial.begin(BAUD_RATE);
    while (!Serial); // wait for ready
    while (Serial.available() && Serial.read()); // empty buffer

    delay(100);
    PTLF("\n* Start *");
    PTLF("Bittle");
    PTLF("Initialize I2C");
    initI2C();
    initIMU();

    irrecv.enableIRIn(); // Start the receiver

    assignSkillAddressToOnboardEeprom();
    PTL();

    // servo
    { 
        pwm.begin();

        pwm.setPWMFreq(60 * PWM_FACTOR); // Analog servos run at ~60 Hz updates
        delay(200);

        //meow();
        for (int8_t i = DOF - 1; i >= 0; i--) {
            // TODO: Investigate loading middleShift, pulsePerDeg and rotationDirection into RAM/ROM
            servoRange[i] = servoAngleRange(i);
            servoCalibs[i] = servoCalib(i);
            calibratedDuty0[i] =  SERVOMIN + PWM_RANGE / 2 + float(middleShift(i) + servoCalibs[i]) * pulsePerDegreeF(i)  * rotationDirection(i) ;
        }
        lastCmd = Command::Command(Command::Simple::Rest);
        doPostureCommand(lastCmd);
        shutServos();
    }
    beep(30);

    pinMode(BATT, INPUT);
    pinMode(BUZZER, OUTPUT);

    meow();

    pixels.begin();           // INITIALIZE NeoPixel strip object (REQUIRED)
    pixels.show();            // Turn OFF all pixels ASAP
    pixels.setBrightness(50); // Set BRIGHTNESS to about 1/5 (max = 255)
    pixels.setPixelColor(0, pixels.Color(255, 0, 0)); //  Set pixel's color (in RAM)
    pixels.show(); 
}



void Bittleet::loop() { 
    
    static bool enableMotion = false;
    static uint8_t frameIndex = 0;
    static byte firstMotionJoint;

    static Command::Move move{Command::Pace::Medium, Command::Direction::Forward};

    static uint32_t lastUs = micros();

    uint32_t deltaUs = micros() - lastUs;
    int currentTask = scheduler.waitUntilNextTask();
    lastUs = micros();

    PTF("task: "); PT(currentTask);
    PTF("\tdeltaT: "); PT(deltaUs); 
    PTF("\tfree memory: "); PT(freeMemory());
    PTL();


    int battAdcReading = analogRead(BATT);
    Status::Battery battState = Battery::state(battAdcReading);
    if (battState.level == Status::BatteryLevel::Low) { 
        PTLF("Low power!");
        beep(15, 50, 50, 3);
        delay(1500); // HOANI TODO: Should be disabling all servos here
    } else {
        switch(currentTask){
            case TASK_ATTITUDE: {
                doAttitudeTask(move, enableMotion, firstMotionJoint, frameIndex);
                break;
            }
            case TASK_MOTION: {
                doMotionTask(enableMotion, skill, firstMotionJoint, frameIndex);
                break;
            }
            case TASK_INPUT: {
                doInputTask(move, enableMotion, firstMotionJoint, frameIndex);
                break;
            }
        }
    }
}

static void doInputTask(Command::Move& move, bool& enableMotion, uint8_t& firstMotionJoint, uint8_t& frameIndex) {
    static Comms::SerialComms serialComms;

    decode_results results;
    if (irrecv.decode(&results)) {
        Command::Command newCmd = Infrared::parseSignal((results.value >> 8), move);
        irrecv.resume(); // receive the next value
        if (newCmd.type() != Command::Type::None) {
            processNewCommand(newCmd, move, enableMotion, firstMotionJoint, frameIndex);
        }
    }
    
    {
        Command::Command newCmd = serialComms.parse(move, currentAng);
        if (newCmd.type() != Command::Type::None) {
            processNewCommand(newCmd, move, enableMotion, firstMotionJoint, frameIndex);
        }
    }

    
}

static void doAttitudeTask(Command::Move& move, bool& enableMotion, uint8_t& firstMotionJoint, uint8_t& frameIndex) {
    Command::Command newCmd = Command::Command();

    if (checkGyro) {
        checkBodyMotion(newCmd);
    }

    processNewCommand(newCmd, move, enableMotion, firstMotionJoint, frameIndex);
}


static void processNewCommand(Command::Command& newCmd, Command::Move& move, bool& enableMotion, uint8_t& firstMotionJoint, uint8_t& frameIndex){
    if (newCmd.type() == Command::Type::Move) {
        if (newCmd.get(move) == false) {
            PTLF("Move Err"); // Unexpected...
            // TODO: Should add an error beep type
        } else {
            enableMotion = true;
        }
    } else if (newCmd.type() == Command::Type::Simple) {
        Command::Simple cmd;
        if (newCmd.get(cmd) == false) {
            PTLF("Simple Err"); // Unexpected...
        } else {
            switch(cmd) {
                case Command::Simple::Rest: {
                    lastCmd = newCmd;
                    doPostureCommand(lastCmd);
                    enableMotion = false;
                    break;
                }
                case Command::Simple::GyroToggle: {
                    checkGyro = !checkGyro;
                    enableMotion = true;
                    break;
                }
                case Command::Simple::Pause: {
                    enableMotion = !enableMotion;
                    if (enableMotion) {
                        newCmd = Command::Command(); // resume last command. TODO - don't know if this works?
                    } else {
                        shutServos();
                    }
                    break;
                }
                case Command::Simple::SaveServoCalibration: {
                    PTLF("save offset");
                    saveCalib(servoCalibs);
                    break;
                }
                case Command::Simple::AbortServoCalibration: {
                    PTLF("aborted");
                    for (byte i = 0; i < DOF; i++) {
                        servoCalibs[i] = servoCalib( i);
                    }
                    break;
                }
                case Command::Simple::ShowJointAngles: { //show the list of current joint anles
                    printRange(DOF);
                    printList(currentAng);
                    break;
                }
            }
        }
    } else if (newCmd.type() == Command::Type::WithArgs) {
        enableMotion = false;
        Command::WithArgs cmd;
        if (newCmd.get(cmd) == false) {
            PTLF("WithArgs Err"); // Unexpected...
        } else {
            switch(cmd.cmd) {
                case Command::ArgType::Calibrate: {
                    PTL();
                    printRange(DOF);
                    printList(servoCalibs);
                    if (lastCmd != newCmd) { //first time entering the calibration function
                        lastCmd = newCmd;
                        loader->load(newCmd, skill);
                        if (skill.type != Skill::Type::Invalid) {
                            transform(skill.spec);
                        }
                        checkGyro = false;
                    }
                    if (cmd.len == 2) {
                        int16_t index = cmd.args[0];
                        int16_t angle = cmd.args[1];
                        // TODO: This appears to allow both absolute and incremental calibration - kind of wierd logic though; might be able to tidy up later.
                        //      - Incremental won't work because we use i8... maybe add incremental calbrate command instead
                        if (angle >= 1001) { // Using 1001 for incremental calibration. 1001 is adding 1 degree, 1002 is adding 2 and 1009 is adding 9 degrees
                            angle = servoCalibs[index] + angle - 1000;
                        } else if (angle <= -1001) { // Using -1001 for incremental calibration. -1001 is removing 1 degree, 1002 is removing 2 and 1009 is removing 9 degrees
                            angle = servoCalibs[index] + angle + 1000;
                        }
                        servoCalibs[index] = angle;
                        int duty = SERVOMIN + PWM_RANGE / 2 + float(middleShift(index)  + servoCalibs[index] + skill.spec[index]) * pulsePerDegreeF(index) * rotationDirection(index);
                        pwm.setPWM(pin(index), 0,  duty);
                    }
                    break;
                }
                case Command::ArgType::MoveSequentially: {
                    const float angleInterval = 0.2;
                    int angleStep = 0;
                    const int16_t joints = cmd.len/2;
                    skill.type = Skill::Type::Posture;
                    for (int16_t i = 0; i < joints; i++) {
                        int16_t index = cmd.args[0];
                        int16_t angle = cmd.args[1];
                        // TODO: This looks like some incremental step logic
                        //      - need to encapsulate duty in a function
                        //      - we can probably simplify this a lot.
                        angleStep = floor((angle - currentAng[index]) / angleInterval);
                        for (int a = 0; a < abs(angleStep); a++) {
                            int duty = SERVOMIN + PWM_RANGE / 2 + float(middleShift(index)  + servoCalibs[index] + currentAng[index] + a * angleInterval * angleStep / abs(angleStep)) * pulsePerDegreeF(index) * rotationDirection(index);
                            pwm.setPWM(pin(index), 0,  duty);
                        }
                        skill.spec[index] = angle;
                        currentAng[index] = angle;
                    }
                    break;
                } 
                case Command::ArgType::Meow: {
                    const int repeat = (cmd.len >= 1) ? cmd.args[0] : 0;
                    const int increment = (cmd.len >= 2) ? cmd.args[1] + 1 : 1;
                    meow(repeat, 0, 50, 200, increment);
                    break;
                }
                case Command::ArgType::Beep: {
                    const int8_t note = (cmd.len >= 1) ? cmd.args[0] : 0;
                    const uint8_t duration = (cmd.len >= 2) ? cmd.args[1] : 0;
                    beep(note, duration);
                    break;
                }
                case Command::ArgType::MoveSimultaneously: {
                    if (cmd.len != DOF) {
                        PTLF("Simultaneous Err"); // Unexpected...
                    } else {
                        transform(cmd.args, 1, 6);
                    }
                    break;
                }
            }
        }
    }

    if (newCmd != Command::Command()) {
        beep(8);
    }

    if ((newCmd != Command::Command()) && (newCmd != lastCmd)) {
        PTL("Loading...");
        loader->load(newCmd, skill);
        PTL("Loaded");

        offsetLR = 0;
        if (newCmd.type() == Command::Type::Move) {
            if (newCmd.get(move)) {
                if (move.direction == Command::Direction::Left) {
                    offsetLR = 15;
                } else if (move.direction == Command::Direction::Right) {
                    offsetLR = -15;
                }
            }
        } 

        frameIndex = 0;

        lastCmd = newCmd;

        postureOrWalkingFactor = (skill.type == Skill::Type::Posture) ? 1 : POSTURE_WALKING_FACTOR;
        firstMotionJoint = (skill.type == Skill::Type::Gait) ? DOF - WALKING_DOF : 0;

        if (skill.type == Skill::Type::Behaviour) {
            doBehaviorSkill(skill);
            lastCmd = Command::Command(Command::Simple::Balance);
            doPostureCommand(lastCmd, 1, 2, false);
            for (byte a = 0; a < DOF; a++) {
                currentAdjust[a] = 0.0f;
            }
        } else if (skill.type != Skill::Type::Invalid) {
            int8_t angleMultiplier = (skill.doubleAngles) ? 2 : 1;
            transform(skill.spec, angleMultiplier, 1, firstMotionJoint);
        }

        if (newCmd == Command::Simple::Rest) {
            shutServos();
            enableMotion = false;
        }
    }
}

static void doMotionTask(bool enableMotion, const Skill::Skill& skill, uint8_t firstMotionJoint, uint8_t& frameIndex) {
    if (enableMotion) {
        doMotionMove(skill, firstMotionJoint, frameIndex);
    } else {
        doMotionPosture(skill);
    }
}


static void doMotionPosture(const Skill::Skill& skill) {
    if (skill.type == Skill::Type::Posture) {
        for (int i = 0; i<DOF; i++) {
            if (i == 1) {
                i = DOF - WALKING_DOF; // Dirty hack here... TODO: Clean up
            }
            if (i == 0) {
                calibratedPWM(i, rollDeviation);
            } else {
                int8_t angleMultiplier = (skill.doubleAngles) ? 2 : 1;
                float attitudeAdjustment = (checkGyro ? adjust(i) : 0.0f);
                calibratedPWM(i, skill.spec[i]*angleMultiplier + attitudeAdjustment);
            }
        }
    }
}

static void doMotionMove(const Skill::Skill& skill, uint8_t firstMotionJoint, uint8_t& frameIndex) {
    if (skill.type == Skill::Type::Gait) {
        if (frameIndex >= skill.frames) {
            frameIndex = 0;
        }

        for (int i = 0; i<DOF; i++) {
            if (i == 0) {
                if (skill.frames > 1) {
                    calibratedPWM(i, offsetLR //look left or right
                                + 10 * sin (frameIndex * (2) * M_PI / skill.frames) //look around
                            );
                }
            } else {
                if (i == 1) {
                    i = firstMotionJoint;
                }
                
                int8_t angleMultiplier = (skill.doubleAngles) ? 2 : 1;
                int dutyIdx = frameIndex * WALKING_DOF + (i - firstMotionJoint);
                calibratedPWM(i, skill.spec[dutyIdx]*angleMultiplier);
            }
        }
        frameIndex++;
    } else {
        frameIndex = 0;
    }
}

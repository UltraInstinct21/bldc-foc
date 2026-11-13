/**
 * @file    encoder.h
 * @brief   Rotor angle / speed handling for a sensored FOC with AS5600.
 *
 * Provides:
 *   - multi-turn mechanical angle and filtered speed
 *   - latency-compensated electrical angle
 *   - one-time offset + direction calibration, stored in the last flash page
 */
#ifndef ENCODER_H
#define ENCODER_H

#include <stdint.h>

/* ---- motor parameters ---------------------------------------------------- */
#ifndef MOTOR_POLE_PAIRS
#define MOTOR_POLE_PAIRS        7u      /* change to match your motor */
#endif

/* ---- calibration record -------------------------------------------------- */
typedef struct
{
    float    sign;          /* +1 or -1, encoder vs. phase order */
    float    offset;        /* electrical radians */
    uint16_t pole_pairs;
    uint8_t  valid;
} EncoderCal;

/* ---- live state (read only from outside) --------------------------------- */
typedef struct
{
    float    theta_mech;    /* continuous mechanical angle, rad (can exceed 2pi) */
    float    omega_mech;    /* filtered mechanical speed, rad/s */
    float    theta_e;       /* last electrical angle, rad 0..2pi */
    uint32_t tick;          /* CPU cycle count of the last sample */
    uint32_t seq;           /* samples consumed */
    uint32_t bad_samples;   /* rejected 0 / 4095 lookups etc. */
    uint8_t  valid;
} EncoderState;

/* Call once after MX_I2C1_Init() and AS5600_Init(). Loads calibration. */
void  Encoder_Init(void);

/* Feed a new raw sample (0..4095). Call from HAL_I2C_MemRxCpltCallback,
 * right after AS5600_RxComplete(). Runs in interrupt context. */
void  Encoder_OnSample(uint16_t raw12);

/* Call from the control ISR. Starts a new I2C read every N calls.
 * N = 5 gives 4 kHz at a 20 kHz control loop. */
void  Encoder_Tick(void);

/* Latency-compensated electrical angle, rad 0..2pi. Call from the control ISR. */
float Encoder_ElectricalAngle(void);

/* Mechanical / electrical speed, rad/s. */
float Encoder_MechanicalSpeed(void);
float Encoder_ElectricalSpeed(void);

const EncoderState *Encoder_GetState(void);
const EncoderCal   *Encoder_GetCal(void);

/* ---- calibration --------------------------------------------------------- */
/**
 * Align the rotor to a known stator vector and compute sign + offset.
 *
 * @param apply_vector  callback that applies a stationary voltage vector:
 *                      apply_vector(theta_electrical_rad, modulation_0_to_1)
 * @param hold_ms       settle time per alignment point (300-500 ms is typical)
 * @param mod           modulation used for alignment. Keep it small: at
 *                      standstill I = mod*Vbus/Rph. 0.01 is ~2.2 A for the
 *                      design motor (Rph = 0.109 ohm, 24 V); 0.05 would be
 *                      ~11 A. FOC_ApplyOpenLoopVector() clamps to 0.02.
 *
 * The rotor must be free to move. The function blocks for
 * ~3 * hold_ms. Saves the result to flash on success.
 *
 * @return 0 on success, negative on failure.
 */
int  Encoder_Calibrate(void (*apply_vector)(float theta_e, float modulation),
                       uint32_t hold_ms, float mod);

/* Flash helpers. */
int  Encoder_LoadCalibration(void);
int  Encoder_SaveCalibration(void);
void Encoder_ResetCalibration(void);

#endif /* ENCODER_H */

/**
 ******************************************************************************
 * @addtogroup OpenPilotModules OpenPilot Modules
 * @{
 * @addtogroup State Estimation
 * @brief Acquires sensor data and computes state estimate
 * @{
 *
 * @file       stateestimation.c
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2013.
 * @brief      Module to handle all comms to the AHRS on a periodic basis.
 *
 * @see        The GNU Public License (GPL) Version 3
 *
 ******************************************************************************/
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include "inc/stateestimation.h"

#include <gyrosensor.h>
#include <accelsensor.h>
#include <magsensor.h>
#include <barosensor.h>
#include <airspeedsensor.h>
#include <gpspositionsensor.h>
#include <gpsvelocitysensor.h>

#include <gyrostate.h>
#include <accelstate.h>
#include <magstate.h>
#include <airspeedstate.h>
#include <attitudestate.h>
#include <positionstate.h>
#include <velocitystate.h>

#include "revosettings.h"
#include "homelocation.h"
#include "flightstatus.h"

#include "CoordinateConversions.h"

// Private constants
#define STACK_SIZE_BYTES  256
#define CALLBACK_PRIORITY CALLBACK_PRIORITY_REGULAR
#define TASK_PRIORITY     CALLBACK_TASK_FLIGHTCONTROL
#define TIMEOUT_MS        100

// Private types

struct filterPipelineStruct;

typedef const struct filterPipelineStruct {
    const stateFilter *filter;
    const struct filterPipelineStruct *next;
} filterPipeline;

// Private variables
static DelayedCallbackInfo *stateEstimationCallback;

static volatile RevoSettingsData revoSettings;
static volatile HomeLocationData homeLocation;
static float LLA2NEDM[3];
static volatile sensorUpdates updatedSensors;
static int32_t fusionAlgorithm     = -1;
static filterPipeline *filterChain = NULL;

// different filters available to state estimation
static stateFilter magFilter;
static stateFilter baroFilter;
static stateFilter airFilter;
static stateFilter stationaryFilter;
static stateFilter cfFilter;
static stateFilter cfmFilter;
static stateFilter ekf13iFilter;
static stateFilter ekf13Filter;
static stateFilter ekf16Filter;
static stateFilter ekf16iFilter;

// preconfigured filter chains selectable via revoSettings.FusionAlgorithm
static filterPipeline *cfQueue = &(filterPipeline) {
    .filter = &magFilter,
    .next   = &(filterPipeline) {
        .filter = &airFilter,
        .next   = &(filterPipeline) {
            .filter = &baroFilter,
            .next   = &(filterPipeline) {
                .filter = &cfFilter,
                .next   = NULL,
            },
        }
    }
};
static const filterPipeline *cfmQueue = &(filterPipeline) {
    .filter = &magFilter,
    .next   = &(filterPipeline) {
        .filter = &airFilter,
        .next   = &(filterPipeline) {
            .filter = &baroFilter,
            .next   = &(filterPipeline) {
                .filter = &cfmFilter,
                .next   = NULL,
            }
        }
    }
};
static const filterPipeline *ekf13iQueue = &(filterPipeline) {
    .filter = &magFilter,
    .next   = &(filterPipeline) {
        .filter = &airFilter,
        .next   = &(filterPipeline) {
            .filter = &baroFilter,
            .next   = &(filterPipeline) {
                .filter = &stationaryFilter,
                .next   = &(filterPipeline) {
                    .filter = &ekf13iFilter,
                    .next   = NULL,
                }
            }
        }
    }
};
static const filterPipeline *ekf13Queue = &(filterPipeline) {
    .filter = &magFilter,
    .next   = &(filterPipeline) {
        .filter = &airFilter,
        .next   = &(filterPipeline) {
            .filter = &baroFilter,
            .next   = &(filterPipeline) {
                .filter = &ekf13Filter,
                .next   = NULL,
            }
        }
    }
};
static const filterPipeline *ekf16iQueue = &(filterPipeline) {
    .filter = &magFilter,
    .next   = &(filterPipeline) {
        .filter = &airFilter,
        .next   = &(filterPipeline) {
            .filter = &baroFilter,
            .next   = &(filterPipeline) {
                .filter = &stationaryFilter,
                .next   = &(filterPipeline) {
                    .filter = &ekf16iFilter,
                    .next   = NULL,
                }
            }
        }
    }
};
static const filterPipeline *ekf16Queue = &(filterPipeline) {
    .filter = &magFilter,
    .next   = &(filterPipeline) {
        .filter = &airFilter,
        .next   = &(filterPipeline) {
            .filter = &baroFilter,
            .next   = &(filterPipeline) {
                .filter = &ekf16Filter,
                .next   = NULL,
            }
        }
    }
};

// Private functions

static void settingsUpdatedCb(UAVObjEvent *objEv);
static void sensorUpdatedCb(UAVObjEvent *objEv);
static void StateEstimationCb(void);
static void getNED(GPSPositionSensorData *gpsPosition, float *NED);
static bool sane(float value);

static inline int32_t maxint32_t(int32_t a, int32_t b)
{
    if (a > b) {
        return a;
    }
    return b;
}

/**
 * Initialise the module.  Called before the start function
 * \returns 0 on success or -1 if initialisation failed
 */
int32_t StateEstimationInitialize(void)
{
    RevoSettingsInitialize();
    HomeLocationInitialize();

    GyroSensorInitialize();
    MagSensorInitialize();
    BaroSensorInitialize();
    AirspeedSensorInitialize();
    GPSPositionSensorInitialize();
    GPSVelocitySensorInitialize();

    GyroStateInitialize();
    AccelStateInitialize();
    MagStateInitialize();
    AirspeedStateInitialize();
    PositionStateInitialize();
    VelocityStateInitialize();

    RevoSettingsConnectCallback(&settingsUpdatedCb);
    HomeLocationConnectCallback(&settingsUpdatedCb);

    GyroSensorConnectCallback(&sensorUpdatedCb);
    AccelSensorConnectCallback(&sensorUpdatedCb);
    MagSensorConnectCallback(&sensorUpdatedCb);
    BaroSensorConnectCallback(&sensorUpdatedCb);
    AirspeedSensorConnectCallback(&sensorUpdatedCb);
    GPSPositionSensorConnectCallback(&sensorUpdatedCb);
    GPSVelocitySensorConnectCallback(&sensorUpdatedCb);

    uint32_t stack_required = STACK_SIZE_BYTES;
    // Initialize Filters
    stack_required = maxint32_t(stack_required, filterMagInitialize(&magFilter));
    stack_required = maxint32_t(stack_required, filterBaroInitialize(&baroFilter));
    stack_required = maxint32_t(stack_required, filterAirInitialize(&airFilter));
    stack_required = maxint32_t(stack_required, filterStationaryInitialize(&stationaryFilter));
    stack_required = maxint32_t(stack_required, filterCFInitialize(&cfFilter));
    stack_required = maxint32_t(stack_required, filterCFMInitialize(&cfmFilter));
    stack_required = maxint32_t(stack_required, filterEKF13iInitialize(&ekf13iFilter));
    stack_required = maxint32_t(stack_required, filterEKF13Initialize(&ekf13Filter));
    stack_required = maxint32_t(stack_required, filterEKF16Initialize(&ekf16Filter));
    stack_required = maxint32_t(stack_required, filterEKF16iInitialize(&ekf16iFilter));

    stateEstimationCallback = DelayedCallbackCreate(&StateEstimationCb, CALLBACK_PRIORITY, TASK_PRIORITY, stack_required);

    return 0;
}

/**
 * Start the task.  Expects all objects to be initialized by this point.
 * \returns 0 on success or -1 if initialisation failed
 */
int32_t StateEstimationStart(void)
{
    RevoSettingsConnectCallback(&settingsUpdatedCb);

    // Force settings update to make sure rotation loaded
    settingsUpdatedCb(NULL);

    return 0;
}

MODULE_INITCALL(StateEstimationInitialize, StateEstimationStart)

/**
 * Module callback
 */
static void StateEstimationCb(void)
{
    static enum { RUNSTATE_LOAD = 0, RUNSTATE_FILTER = 1, RUNSTATE_SAVE = 2 } runState = RUNSTATE_LOAD;
    static int8_t alarm     = 0;
    static int8_t lastAlarm = -1;
    static uint16_t alarmcounter = 0;
    static filterPipeline *current;
    static stateEstimation states;

    switch (runState) {
    case RUNSTATE_LOAD:

        alarm = 0;

        // set alarm to warning if called through timeout
        if (updatedSensors == 0) {
            alarm = 1;
        }

        // check if a new filter chain should be initialized
        if (fusionAlgorithm != revoSettings.FusionAlgorithm) {
            FlightStatusData fs;
            FlightStatusGet(&fs);
            if (fs.Armed == FLIGHTSTATUS_ARMED_DISARMED || fusionAlgorithm == -1) {
                const filterPipeline *newFilterChain;
                switch (revoSettings.FusionAlgorithm) {
                case REVOSETTINGS_FUSIONALGORITHM_COMPLEMENTARY:
                    newFilterChain = cfQueue;
                    break;
                case REVOSETTINGS_FUSIONALGORITHM_COMPLEMENTARYMAG:
                    newFilterChain = cfmQueue;
                    break;
                case REVOSETTINGS_FUSIONALGORITHM_INS13INDOOR:
                    newFilterChain = ekf13iQueue;
                    break;
                case REVOSETTINGS_FUSIONALGORITHM_INS13OUTDOOR:
                    newFilterChain = ekf13Queue;
                    break;
                case REVOSETTINGS_FUSIONALGORITHM_INS16INDOOR:
                    newFilterChain = ekf16iQueue;
                    break;
                case REVOSETTINGS_FUSIONALGORITHM_INS16OUTDOOR:
                    newFilterChain = ekf16Queue;
                    break;
                default:
                    newFilterChain = NULL;
                }
                // initialize filters in chain
                current = (filterPipeline *)newFilterChain;
                bool error = 0;
                while (current != NULL) {
                    int32_t result = current->filter->init((stateFilter *)current->filter);
                    if (result != 0) {
                        error = 1;
                        break;
                    }
                    current = current->next;
                }
                if (error) {
                    AlarmsSet(SYSTEMALARMS_ALARM_ATTITUDE, SYSTEMALARMS_ALARM_ERROR);
                    return;
                } else {
                    // set new fusion algortithm
                    filterChain     = (filterPipeline *)newFilterChain;
                    fusionAlgorithm = revoSettings.FusionAlgorithm;
                }
            }
        }

        // read updated sensor UAVObjects and set initial state
        states.updated  = updatedSensors;
        updatedSensors ^= states.updated;

        // most sensors get only rudimentary sanity checks
#define SANITYCHECK3(sensorname, shortname, a1, a2, a3) \
    if (ISSET(states.updated, SENSORUPDATES_##shortname)) { \
        sensorname##Data s; \
        sensorname##Get(&s); \
        if (sane(s.a1) && sane(s.a2) && sane(s.a3)) { \
            states.shortname[0] = s.a1; \
            states.shortname[1] = s.a2; \
            states.shortname[2] = s.a3; \
        } \
        else { \
            UNSET(states.updated, SENSORUPDATES_##shortname); \
        } \
    }
        SANITYCHECK3(GyroSensor, gyro, x, y, z);
        SANITYCHECK3(AccelSensor, accel, x, y, z);
        SANITYCHECK3(MagSensor, mag, x, y, z);
        SANITYCHECK3(GPSVelocitySensor, vel, North, East, Down);
#define SANITYCHECK1(sensorname, shortname, a1, EXTRACHECK) \
    if (ISSET(states.updated, SENSORUPDATES_##shortname)) { \
        sensorname##Data s; \
        sensorname##Get(&s); \
        if (sane(s.a1) && EXTRACHECK) { \
            states.shortname[0] = s.a1; \
        } \
        else { \
            UNSET(states.updated, SENSORUPDATES_##shortname); \
        } \
    }
        SANITYCHECK1(BaroSensor, baro, Altitude, 1);
        SANITYCHECK1(AirspeedSensor, airspeed, CalibratedAirspeed, s.SensorConnected == AIRSPEEDSENSOR_SENSORCONNECTED_TRUE);
        states.airspeed[1] = 0.0f; // sensor does not provide true airspeed, needs to be calculated by filter

        // GPS is a tiny bit more tricky as GPSPositionSensor is not float (otherwise the conversion to NED could sit in a filter) but integers, for precision reasons
        if (ISSET(states.updated, SENSORUPDATES_pos)) {
            GPSPositionSensorData s;
            GPSPositionSensorGet(&s);
            if (homeLocation.Set == HOMELOCATION_SET_TRUE && sane(s.Latitude) && sane(s.Longitude) && sane(s.Altitude) && (fabsf(s.Latitude) > 1e-5f || fabsf(s.Latitude) > 1e-5f || fabsf(s.Latitude) > 1e-5f)) {
                getNED(&s, states.pos);
            } else {
                UNSET(states.updated, SENSORUPDATES_pos);
            }
        }

        // at this point sensor state is stored in "states" with some rudimentary filtering applied

        // apply all filters in the current filter chain
        current  = (filterPipeline *)filterChain;

        // we are not done, re-dispatch self execution
        runState = RUNSTATE_FILTER;
        DelayedCallbackDispatch(stateEstimationCallback);
        break;

    case RUNSTATE_FILTER:

        if (current != NULL) {
            int32_t result = current->filter->filter((stateFilter *)current->filter, &states);
            if (result > alarm) {
                alarm = result;
            }
            current = current->next;
        }

        // we are not done, re-dispatch self execution
        if (!current) {
            runState = RUNSTATE_SAVE;
        }
        DelayedCallbackDispatch(stateEstimationCallback);
        break;

    case RUNSTATE_SAVE:

        // the final output of filters is saved in state variables
#define STORE3(statename, shortname, a1, a2, a3) \
    if (ISSET(states.updated, SENSORUPDATES_##shortname)) { \
        statename##Data s; \
        statename##Get(&s); \
        s.a1 = states.shortname[0]; \
        s.a2 = states.shortname[1]; \
        s.a3 = states.shortname[2]; \
        statename##Set(&s); \
    }
        STORE3(GyroState, gyro, x, y, z);
        STORE3(AccelState, accel, x, y, z);
        STORE3(MagState, mag, x, y, z);
        STORE3(PositionState, pos, North, East, Down);
        STORE3(VelocityState, vel, North, East, Down);
#define STORE2(statename, shortname, a1, a2) \
    if (ISSET(states.updated, SENSORUPDATES_##shortname)) { \
        statename##Data s; \
        statename##Get(&s); \
        s.a1 = states.shortname[0]; \
        s.a2 = states.shortname[1]; \
        statename##Set(&s); \
    }
        STORE2(AirspeedState, airspeed, CalibratedAirspeed, TrueAirspeed);
        // attitude nees manual conversion from quaternion to euler
        if (ISSET(states.updated, SENSORUPDATES_attitude)) { \
            AttitudeStateData s;
            AttitudeStateGet(&s);
            s.q1 = states.attitude[0];
            s.q2 = states.attitude[1];
            s.q3 = states.attitude[2];
            s.q4 = states.attitude[3];
            Quaternion2RPY(&s.q1, &s.Roll);
            AttitudeStateSet(&s);
        }

        // throttle alarms, raise alarm flags immediately
        // but require system to run for a while before decreasing
        // to prevent alarm flapping
        if (alarm >= lastAlarm) {
            lastAlarm    = alarm;
            alarmcounter = 0;
        } else {
            if (alarmcounter < 100) {
                alarmcounter++;
            } else {
                lastAlarm    = alarm;
                alarmcounter = 0;
            }
        }

        // clear alarms if everything is alright, then schedule callback execution after timeout
        if (lastAlarm == 1) {
            AlarmsSet(SYSTEMALARMS_ALARM_ATTITUDE, SYSTEMALARMS_ALARM_WARNING);
        } else if (lastAlarm == 2) {
            AlarmsSet(SYSTEMALARMS_ALARM_ATTITUDE, SYSTEMALARMS_ALARM_ERROR);
        } else if (lastAlarm >= 3) {
            AlarmsSet(SYSTEMALARMS_ALARM_ATTITUDE, SYSTEMALARMS_ALARM_CRITICAL);
        } else {
            AlarmsClear(SYSTEMALARMS_ALARM_ATTITUDE);
        }

        // we are done, re-schedule next self execution
        runState = RUNSTATE_LOAD;
        if (updatedSensors) {
            DelayedCallbackDispatch(stateEstimationCallback);
        } else {
            DelayedCallbackSchedule(stateEstimationCallback, TIMEOUT_MS, CALLBACK_UPDATEMODE_SOONER);
        }
        break;
    }
}


/**
 * Callback for eventdispatcher when HomeLocation or RevoSettings has been updated
 */
static void settingsUpdatedCb(__attribute__((unused)) UAVObjEvent *ev)
{
    HomeLocationGet((HomeLocationData *)&homeLocation);

    if (sane(homeLocation.Latitude) && sane(homeLocation.Longitude) && sane(homeLocation.Altitude) && sane(homeLocation.Be[0]) && sane(homeLocation.Be[1]) && sane(homeLocation.Be[2])) {
        // Compute matrix to convert deltaLLA to NED
        float lat, alt;
        lat = DEG2RAD(homeLocation.Latitude / 10.0e6f);
        alt = homeLocation.Altitude;

        LLA2NEDM[0] = alt + 6.378137E6f;
        LLA2NEDM[1] = cosf(lat) * (alt + 6.378137E6f);
        LLA2NEDM[2] = -1.0f;

        // TODO: convert positionState to new reference frame and gracefully update EKF state!
        // needed for long range flights where the reference coordinate is adjusted in flight
    }

    RevoSettingsGet((RevoSettingsData *)&revoSettings);
}

/**
 * Callback for eventdispatcher when any sensor UAVObject has been updated
 * updates the list of "recently updated UAVObjects" and dispatches the state estimator callback
 */
static void sensorUpdatedCb(UAVObjEvent *ev)
{
    if (!ev) {
        return;
    }

    if (ev->obj == GyroSensorHandle()) {
        updatedSensors |= SENSORUPDATES_gyro;
    }

    if (ev->obj == AccelSensorHandle()) {
        updatedSensors |= SENSORUPDATES_accel;
    }

    if (ev->obj == MagSensorHandle()) {
        updatedSensors |= SENSORUPDATES_mag;
    }

    if (ev->obj == GPSPositionSensorHandle()) {
        updatedSensors |= SENSORUPDATES_pos;
    }

    if (ev->obj == GPSVelocitySensorHandle()) {
        updatedSensors |= SENSORUPDATES_vel;
    }

    if (ev->obj == BaroSensorHandle()) {
        updatedSensors |= SENSORUPDATES_baro;
    }

    if (ev->obj == AirspeedSensorHandle()) {
        updatedSensors |= SENSORUPDATES_airspeed;
    }

    DelayedCallbackDispatch(stateEstimationCallback);
}

/**
 * @brief Convert the GPS LLA position into NED coordinates
 * @note this method uses a taylor expansion around the home coordinates
 * to convert to NED which allows it to be done with all floating
 * calculations
 * @param[in] Current GPS coordinates
 * @param[out] NED frame coordinates
 * @returns 0 for success, -1 for failure
 */
static void getNED(GPSPositionSensorData *gpsPosition, float *NED)
{
    float dL[3] = { DEG2RAD((gpsPosition->Latitude - homeLocation.Latitude) / 10.0e6f),
                    DEG2RAD((gpsPosition->Longitude - homeLocation.Longitude) / 10.0e6f),
                    (gpsPosition->Altitude + gpsPosition->GeoidSeparation - homeLocation.Altitude) };

    NED[0] = LLA2NEDM[0] * dL[0];
    NED[1] = LLA2NEDM[1] * dL[1];
    NED[2] = LLA2NEDM[2] * dL[2];
}

/**
 * @brief sanity check for float values
 * @note makes sure a float value is safe for further processing, ie not NAN and not INF
 * @param[in] float value
 * @returns true for safe and false for unsafe
 */
static bool sane(float value)
{
    if (isnan(value) || isinf(value)) {
        return false;
    }
    return true;
}

/**
 * @}
 * @}
 */
#ifndef CONTROLSTATEMACHINE_H

#define CONTROLSTATEMACHINE_H

#include "stm32f7xx_hal.h"
#include "state_machine.h"

typedef enum BMU_States_t {
    STATE_Self_Check = 0,
    STATE_HV_Disable,
    STATE_HV_Enable,
    STATE_Precharge,
    STATE_Discharge,
    STATE_Failure_Fatal,
    STATE_ANY, // Must be the last state
} BMU_States_t;

typedef enum BMU_Events_t {
    EV_Init = 0,
    EV_HV_Toggle,
    EV_Precharge_Finished,
    EV_Discharge_Finished,
    EV_PrechargeDischarge_Fail,
    EV_HV_Fault,
    EV_ANY, // Must be the last event
} BMU_Events_t;

extern FSM_Handle_Struct fsmHandle;

HAL_StatusTypeDef controlInit(void);
HAL_StatusTypeDef startControl();
void controlTask(void *pvParameters);

#endif /* end of include guard: CONTROLSTATEMACHINE_H */
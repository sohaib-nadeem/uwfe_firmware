#include "controlStateMachine.h"

#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os.h"
#include "stdbool.h"

#include "bsp.h"
#include "debug.h"
#include "dcu_can.h"
#include "userCan.h"
#include "watchdog.h"
#include "canReceive.h"

#define MAIN_TASK_ID 1
#define MAIN_TASK_PERIOD_MS 1000

extern osThreadId mainTaskHandle;

#define BUZZER_LENGTH_MS 1000
TimerHandle_t buzzerSoundTimer;

void buzzerTimerCallback(TimerHandle_t timer);
bool buzzerTimerStarted = false;

#define DEBOUNCE_WAIT_MS 50

TimerHandle_t debounceTimer;

void debounceTimerCallback(TimerHandle_t timer);
bool debounceTimerStarted = false;

uint32_t toggleHV(uint32_t event);
uint32_t toggleEM(uint32_t event);
uint32_t hvControl(uint32_t event);
uint32_t emControl(uint32_t event);
uint32_t defaultTransition(uint32_t event);
void mainTaskFunction(void const * argument);

Transition_t transitions[] = {
    {STATE_HV_Disable, EV_HV_Toggle,   &toggleHV},
    {STATE_HV_Toggle,  EV_CAN_Recieve, &hvControl},
    {STATE_HV_Enable,  EV_EM_Toggle,   &toggleEM},
    {STATE_EM_Toggle,  EV_CAN_Recieve, &emControl},
    {STATE_EM_Enable,  EV_EM_Toggle,   &toggleEM},
    {STATE_HV_Enable,  EV_HV_Toggle,   &toggleHV},
    {STATE_ANY,        EV_ANY,         &defaultTransition}
};

int sendHVToggleMsg(void)
{
    ButtonHVEnabled = 1;
    ButtonEMEnabled = 0;
    return sendCAN_DCU_buttonEvents();
}

int sendEMToggleMsg(void)
{
    ButtonHVEnabled = 0;
    ButtonEMEnabled = 1;
    return sendCAN_DCU_buttonEvents();
}

uint32_t toggleHV(uint32_t event)
{
    DEBUG_PRINT("Sending HV Toggle button event\n");
    if (sendHVToggleMsg() != HAL_OK)
    {
        ERROR_PRINT("Failed to send HV Toggle button event!\n");
        Error_Handler();
    }

    return STATE_HV_Toggle;
}

uint32_t toggleEM(uint32_t event)
{
    if (fsmGetState(&DCUFsmHandle) == STATE_HV_Enable)
    {
        /* Only ring buzzer when going to motors enabled */
        DEBUG_PRINT("Kicking off buzzer\n");
        if (!buzzerTimerStarted)
        {
            if (xTimerStart(buzzerSoundTimer, 100) != pdPASS)
            {
                ERROR_PRINT("Failed to start buzzer timer\n");
                Error_Handler();
            }

            buzzerTimerStarted = true;
            BUZZER_ON
        }
    }

    DEBUG_PRINT("Sending EM Toggle button event\n");
    if (sendEMToggleMsg() != HAL_OK)
    {
        ERROR_PRINT("Failed to send EM Toggle button event!\n");
        Error_Handler();
    }

    return STATE_EM_Toggle;
}

uint32_t hvControl(uint32_t event)
{
    if(getHVState() == HV_Power_State_On)
    {
        DEBUG_PRINT("Response from BMU: HV Enabled\n");
        return STATE_HV_Enable;
    }
    else
    {
        DEBUG_PRINT("Response from BMU: HV Disabled\n");
        return STATE_HV_Disable;
    }
}

uint32_t emControl(uint32_t event)
{
    if(getEMState() == EM_State_On)
    {
        DEBUG_PRINT("Response from VCU: EM Enabled\n");
        return STATE_EM_Enable;
    }
    else
    {
        DEBUG_PRINT("Response from VCU: EM Disabled\n");
        return STATE_HV_Enable;
    }
}

bool alreadyDebouncing = false;
uint16_t debouncingPin = 0;

/*
 * A button press is considered valid if it is still low after TIMER_WAIT_MS
 * milliseconds.
 */
void debounceTimerCallback(TimerHandle_t timer)
{
    GPIO_PinState pin_val;

    switch (debouncingPin)
    {
        case HV_TOGGLE_BUTTON_PIN:
            pin_val = HAL_GPIO_ReadPin(HV_TOGGLE_BUTTON_PORT,
                    HV_TOGGLE_BUTTON_PIN);
            break;
        
        case EM_TOGGLE_BUTTON_PIN:
            pin_val = HAL_GPIO_ReadPin(EM_TOGGLE_BUTTON_PORT,
                    EM_TOGGLE_BUTTON_PIN);
            break;

        default:
            /* Shouldn't get here */ 
            DEBUG_PRINT_ISR("Unknown pin specified to debounce\n");
            pin_val = GPIO_PIN_SET;
    }

    if (pin_val == GPIO_PIN_RESET)
    {
        switch (debouncingPin)
        {
            case HV_TOGGLE_BUTTON_PIN:
                fsmSendEventISR(&DCUFsmHandle, EV_HV_Toggle);
                break;
            
            case EM_TOGGLE_BUTTON_PIN:
                fsmSendEventISR(&DCUFsmHandle, EV_EM_Toggle);
                break;

            default:
                /* Shouldn't get here */
                DEBUG_PRINT_ISR("Unknown pin specified to debounce\n");
        }

    }

    alreadyDebouncing = false;
}

void HAL_GPIO_EXTI_Callback(uint16_t pin)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    if (alreadyDebouncing)
    {
        /* Already debouncing, do nothing with this interrupt */
        return;
    }

    alreadyDebouncing = true;

    switch (pin)
    {
        case HV_TOGGLE_BUTTON_PIN:
            debouncingPin = HV_TOGGLE_BUTTON_PIN;
            break;

        case EM_TOGGLE_BUTTON_PIN:
            debouncingPin = EM_TOGGLE_BUTTON_PIN;
            break;

        default:
            /* Not a fatal error here, but report error and return */
            DEBUG_PRINT_ISR("Unknown GPIO interrupted in ISR!\n");
            return;
    }
    
    xTimerStartFromISR(debounceTimer, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

HAL_StatusTypeDef dcuFsmInit(){
    FSM_Init_Struct init;
    init.maxStateNum = STATE_ANY;
    init.maxEventNum = EV_ANY;
    init.sizeofEventEnumType = sizeof(DCU_Events_t);
    init.ST_ANY = STATE_ANY;
    init.EV_ANY = EV_ANY;
    init.transitions = transitions;
    init.transitionTableLength = TRANS_COUNT(transitions);
    init.eventQueueLength = 5;
    init.watchdogTaskId = MAIN_TASK_ID;
    if (fsmInit(STATE_HV_Disable, &init, &DCUFsmHandle) != HAL_OK) {
        ERROR_PRINT("Failed to init DCU fsm\n");
        return HAL_ERROR;
    }

    DEBUG_PRINT("Init DCU fsm\n");
    return HAL_OK;

}

void buzzerTimerCallback(TimerHandle_t timer)
{
    buzzerTimerStarted = false;
    BUZZER_OFF
}

void mainTaskFunction(void const * argument){
    DEBUG_PRINT("Starting up!!\n");
    if (canStart(&CAN_HANDLE) != HAL_OK)
    {
        ERROR_PRINT("Failed to start CAN!\n");
        Error_Handler();
    }

    buzzerSoundTimer = xTimerCreate("BuzzerTimer",
                                    pdMS_TO_TICKS(BUZZER_LENGTH_MS),
                                    pdFALSE /* Auto Reload */,
                                    0,
                                    buzzerTimerCallback);

    if (buzzerSoundTimer == NULL)
    {
        ERROR_PRINT("Failed to create buzzer timer!\n");
        Error_Handler();
    }

    debounceTimer = xTimerCreate("DebounceTimer",
                                 pdMS_TO_TICKS(DEBOUNCE_WAIT_MS),
                                 pdFALSE /* Auto Reload */,
                                 0,
                                 debounceTimerCallback);

    if (debounceTimer == NULL) {
        ERROR_PRINT("Failed to create debounce timer!\n");
        Error_Handler();
    }


    if (registerTaskToWatch(MAIN_TASK_ID, 5*pdMS_TO_TICKS(MAIN_TASK_PERIOD_MS), true, &DCUFsmHandle) != HAL_OK)
    {
        ERROR_PRINT("Failed to register main task with watchdog!\n");
        Error_Handler();
    }

    fsmTaskFunction(&DCUFsmHandle);

    for(;;);
}

uint32_t defaultTransition(uint32_t event)
{
    ERROR_PRINT("No transition function registered for state %lu, event %lu\n",
                fsmGetState(&DCUFsmHandle), event);

    return fsmGetState(&DCUFsmHandle);
}


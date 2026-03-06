################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (13.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
C:/Work/Projects/ST\ Workspace/LR_BLE_Leak\ Sensor/BLE_HR_P2PServer/Projects/Common/WPAN/Modules/SerialCmdInterpreter/serial_cmd_interpreter.c 

OBJS += \
./Common/WPAN/Modules/SerialCmdInterpreter/serial_cmd_interpreter.o 

C_DEPS += \
./Common/WPAN/Modules/SerialCmdInterpreter/serial_cmd_interpreter.d 


# Each subdirectory must supply rules for building sources it contributes
Common/WPAN/Modules/SerialCmdInterpreter/serial_cmd_interpreter.o: C:/Work/Projects/ST\ Workspace/LR_BLE_Leak\ Sensor/BLE_HR_P2PServer/Projects/Common/WPAN/Modules/SerialCmdInterpreter/serial_cmd_interpreter.c Common/WPAN/Modules/SerialCmdInterpreter/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m33 -std=gnu11 -g3 -DDEBUG -DUSE_FULL_LL_DRIVER -DBLE_LL -DUSE_HAL_DRIVER -DSTM32WBA5Mxx -DPHY_40nm_3_00_a -DBLE_STACK_BASIC_PLUS_FEATURES -c -I../../Core/Inc -I../../System/Interfaces -I../../System/Modules -I../../System/Config -I../../System/Config/Log -I../../System/Config/LowPower -I../../System/Config/Debug_GPIO -I../../System/Config/Flash -I../../System/Config/ADC_Ctrl -I../../System/Config/CRC_Ctrl -I../../STM32_WPAN/App -I../../STM32_WPAN/Target -I../../Drivers/STM32WBAxx_HAL_Driver/Inc -I../../Drivers/STM32WBAxx_HAL_Driver/Inc/Legacy -I../../Utilities/trace/adv_trace -I../../Projects/Common/WPAN/Interfaces -I../../Projects/Common/WPAN/Modules -I../../Projects/Common/WPAN/Modules/BasicAES -I../../Projects/Common/WPAN/Modules/Flash -I../../Projects/Common/WPAN/Modules/MemoryManager -I../../Projects/Common/WPAN/Modules/Nvm -I../../Projects/Common/WPAN/Modules/RTDebug -I../../Projects/Common/WPAN/Modules/SerialCmdInterpreter -I../../Projects/Common/WPAN/Modules/Log -I../../Utilities/misc -I../../Utilities/sequencer -I../../Utilities/tim_serv -I../../Utilities/lpm/tiny_lpm -I../../Middlewares/ST/STM32_WPAN -I../../Middlewares/ST/STM32_WPAN/link_layer/ll_cmd_lib/config/ble_basic_plus -I../../Middlewares/ST/STM32_WPAN/ble/svc/Src -I../../Drivers/CMSIS/Device/ST/STM32WBAxx/Include -I../../Middlewares/ST/STM32_WPAN/link_layer/ll_cmd_lib/inc -I../../Middlewares/ST/STM32_WPAN/link_layer/ll_cmd_lib/inc/_40nm_reg_files -I../../Middlewares/ST/STM32_WPAN/link_layer/ll_cmd_lib/inc/ot_inc -I../../Middlewares/ST/STM32_WPAN/link_layer/ll_cmd_lib/porting -I../../Middlewares/ST/STM32_WPAN/link_layer/ll_sys/inc -I../../Middlewares/ST/STM32_WPAN/link_layer/ll_cmd_lib/src/shrd_utils/inc -I../../Middlewares/ST/STM32_WPAN/ble -I../../Middlewares/ST/STM32_WPAN/ble/stack/include -I../../Middlewares/ST/STM32_WPAN/ble/stack/include/auto -I../../Middlewares/ST/STM32_WPAN/ble/svc/Inc -I../../Drivers/CMSIS/Include -I../../Drivers/BSP/B-WBA5M-WPAN -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"Common/WPAN/Modules/SerialCmdInterpreter/serial_cmd_interpreter.d" -MT"$@" --specs=nano.specs -mfpu=fpv5-sp-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-Common-2f-WPAN-2f-Modules-2f-SerialCmdInterpreter

clean-Common-2f-WPAN-2f-Modules-2f-SerialCmdInterpreter:
	-$(RM) ./Common/WPAN/Modules/SerialCmdInterpreter/serial_cmd_interpreter.cyclo ./Common/WPAN/Modules/SerialCmdInterpreter/serial_cmd_interpreter.d ./Common/WPAN/Modules/SerialCmdInterpreter/serial_cmd_interpreter.o ./Common/WPAN/Modules/SerialCmdInterpreter/serial_cmd_interpreter.su

.PHONY: clean-Common-2f-WPAN-2f-Modules-2f-SerialCmdInterpreter


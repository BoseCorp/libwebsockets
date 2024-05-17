include_guard(GLOBAL)
message("${CMAKE_CURRENT_LIST_FILE} lib is included.")


add_subdirectory(${CMAKE_CURRENT_LIST_DIR} EXCLUDE_FROM_ALL)
target_include_directories(websockets PRIVATE
                        ${CORE_DIR}/Source/lwip/src/include/lwip
                        ${CORE_DIR}/Source/NXP/middleware/lwip/src/include
                        ${CORE_DIR}/Source/NXP/middleware/lwip/port
                        ${CORE_DIR}/Source/NXP/CMSIS/Core/Include
                        ${CORE_DIR}/Source/NXP/devices/MIMXRT1064/utilities/debug_console
                        ${CORE_DIR}/Source/NXP/devices/MIMXRT1064/drivers
                        ${CORE_DIR}/Source/NXP/devices/MIMXRT1064
                        ${CORE_DIR}/Source/NXP/components/serial_manager
                        ${CORE_DIR}/Source/NXP/components/uart
                        ${CORE_DIR}/Source/NXP/components/osa
                        ${CORE_DIR}/Source/NXP/components/lists
                        ${CORE_DIR}/Source/NXP/rtos/freertos/freertos-kernel/include
                        ${CORE_DIR}/Source/Bose/Common
                        ${CORE_DIR}/Source/Bose/Logging
                        ${CORE_DIR}/Source/NXP/rtos/freertos/freertos-kernel/portable/GCC/ARM_CM4F
                        )

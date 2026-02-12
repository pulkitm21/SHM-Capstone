# See the build system documentation in IDF programming guide
# for more information about component CMakeLists.txt files.

idf_component_register(
    SRCS main.c
         ethernet.c
         i2c_bus.c
         spi_bus.c
         adt7420.c
         adxl355.c
         scl3300.c
         sensor_task.c
         mqtt.c
         data_processing_and_mqtt_task.c
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS
    REQUIRES esp_driver_i2c
    		 esp_driver_spi
    PRIV_REQUIRES esp_driver_gpio
    			   esp_driver_gptimer 
    			   esp_timer 
    			   freertos
    			   esp_eth
                  esp_netif
                  esp_event
                  ethernet_init
                  nvs_flash
                  mqtt
)
#include "servo_head.h"

#include <cstring>
#include <atomic>
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"

static const char *TAG = "SERVO_HEAD";
static constexpr gpio_num_t HEAD_GPIO = GPIO_NUM_8;
static constexpr gpio_num_t NOD_GPIO = GPIO_NUM_9;
static constexpr uint32_t PWM_FREQ_HZ = 50;
static constexpr uint32_t PWM_PERIOD_US = 20000;
static constexpr uint32_t PWM_MAX_DUTY = (1U << LEDC_TIMER_14_BIT) - 1U;
static constexpr uint16_t NEUTRAL_US = 1500;
static constexpr uint16_t HEAD_LEFT_US = 1450, HEAD_RIGHT_US = 1550;
static constexpr uint16_t NOD_FORWARD_US = 1550, NOD_BACKWARD_US = 1450;
static constexpr uint32_t MICRO_MOVE_MS = 300, GENTLE_PAUSE_MS = 350;
static constexpr uint32_t AUTO_HEAD_MOVE_MS = 85, AUTO_NOD_MOVE_MS = 90;
static constexpr uint32_t AUTO_PAUSES_MS[] = {850,1250,950,1500};
static constexpr TickType_t SERVO_TICK = (pdMS_TO_TICKS(10)>0)?pdMS_TO_TICKS(10):1;
static constexpr ledc_channel_t HEAD_CHANNEL=LEDC_CHANNEL_2, NOD_CHANNEL=LEDC_CHANNEL_3;
enum class ServoAction:uint8_t {LookLeft,LookRight,Nod,Shake};
struct ServoCommand {ServoAction action; uint32_t generation;};
static QueueHandle_t command_queue=nullptr;
static std::atomic<uint32_t> cancel_generation{0};
static std::atomic<bool> ai_speaking{false};
// Shared gaze direction for every servo movement; OLED1 reads this source.
static std::atomic<int> gaze_dir_x{0}, gaze_dir_y{0};
static std::atomic<uint32_t> gaze_transition_ms{0}, gaze_start_tick{0};
static std::atomic<uint32_t> gaze_revision{0};
static portMUX_TYPE gaze_mux = portMUX_INITIALIZER_UNLOCKED;
static void set_gaze_direction(int x, int y, uint32_t transition_ms) {
 const uint32_t start_tick = (uint32_t)xTaskGetTickCount();
 // Keep direction, duration, timestamp and revision as one consistent snapshot.
 portENTER_CRITICAL(&gaze_mux);
 gaze_dir_x.store(x);
 gaze_dir_y.store(y);
 gaze_transition_ms.store(transition_ms);
 gaze_start_tick.store(start_tick);
 gaze_revision.fetch_add(1);
 portEXIT_CRITICAL(&gaze_mux);
}
static bool initialized=false;
static bool set_pulse(ledc_channel_t c,uint16_t us) {
 uint32_t duty=(uint64_t(us)*PWM_MAX_DUTY)/PWM_PERIOD_US;
 esp_err_t e=ledc_set_duty(LEDC_LOW_SPEED_MODE,c,duty);
 if(e==ESP_OK)e=ledc_update_duty(LEDC_LOW_SPEED_MODE,c);
 if(e!=ESP_OK)ESP_LOGE(TAG,"PWM gagal: %s",esp_err_to_name(e));
 return e==ESP_OK;
}
static void stop_channel(ledc_channel_t c){(void)set_pulse(c,NEUTRAL_US);}
static bool cancelled(uint32_t g){return cancel_generation.load()!=g;}
static bool wait_cancellable(uint32_t ms,uint32_t g){
 TickType_t start=xTaskGetTickCount(),dur=pdMS_TO_TICKS(ms);
 while((xTaskGetTickCount()-start)<dur){if(cancelled(g))return false;vTaskDelay(SERVO_TICK);}
 return !cancelled(g);
}
static bool run_segment(ledc_channel_t c,uint16_t pulse,uint32_t ms,uint32_t g){
 if(cancelled(g)||!set_pulse(c,pulse))return false;
 bool ok=wait_cancellable(ms,g); stop_channel(c); return ok;
}
static void execute_manual(const ServoCommand& c){
 bool ok=true;
 switch(c.action){
 case ServoAction::LookLeft:
  set_gaze_direction(-3,0,MICRO_MOVE_MS);
  ok=run_segment(HEAD_CHANNEL,HEAD_LEFT_US,MICRO_MOVE_MS,c.generation);
  break;
 case ServoAction::LookRight:
  set_gaze_direction(3,0,MICRO_MOVE_MS);
  ok=run_segment(HEAD_CHANNEL,HEAD_RIGHT_US,MICRO_MOVE_MS,c.generation);
  break;
 case ServoAction::Nod:
  set_gaze_direction(0,-2,MICRO_MOVE_MS);
  ok=run_segment(NOD_CHANNEL,NOD_FORWARD_US,MICRO_MOVE_MS,c.generation);
  if(ok)ok=wait_cancellable(GENTLE_PAUSE_MS,c.generation);
  if(ok){
   set_gaze_direction(0,2,MICRO_MOVE_MS);
   ok=run_segment(NOD_CHANNEL,NOD_BACKWARD_US,MICRO_MOVE_MS,c.generation);
  }
  break;
 case ServoAction::Shake:
  set_gaze_direction(-3,0,MICRO_MOVE_MS);
  ok=run_segment(HEAD_CHANNEL,HEAD_LEFT_US,MICRO_MOVE_MS,c.generation);
  if(ok)ok=wait_cancellable(GENTLE_PAUSE_MS,c.generation);
  if(ok){
   set_gaze_direction(3,0,MICRO_MOVE_MS);
   ok=run_segment(HEAD_CHANNEL,HEAD_RIGHT_US,MICRO_MOVE_MS,c.generation);
  }
  break;
 }
 stop_channel(c.action==ServoAction::Nod?NOD_CHANNEL:HEAD_CHANNEL);
 set_gaze_direction(0,0,85);
 if(!ok)ESP_LOGW(TAG,"Gerakan dibatalkan/gagal");
}
static void servo_worker(void*){
 ServoCommand c{};uint8_t step=0;
 for(;;){
  if(xQueueReceive(command_queue,&c,0)==pdTRUE){if(!cancelled(c.generation))execute_manual(c);continue;}
  if(!ai_speaking.load()){vTaskDelay(pdMS_TO_TICKS(20));continue;}
  uint32_t g=cancel_generation.load(),pause=AUTO_PAUSES_MS[step];bool ok=false;
  // Publish the direction for OLED1 for every automatic servo step.
  // Do not change any servo pulse, duration, or pause calibration.
  switch(step){
   case 0:set_gaze_direction(-3,0,AUTO_HEAD_MOVE_MS);break;
   case 1:set_gaze_direction(0,-2,AUTO_NOD_MOVE_MS);break;
   case 2:set_gaze_direction(3,0,AUTO_HEAD_MOVE_MS);break;
   default:set_gaze_direction(0,2,AUTO_NOD_MOVE_MS);break;
  }
  switch(step){
   case 0:ok=run_segment(HEAD_CHANNEL,HEAD_LEFT_US,AUTO_HEAD_MOVE_MS,g);break;
   case 1:ok=run_segment(NOD_CHANNEL,NOD_FORWARD_US,AUTO_NOD_MOVE_MS,g);break;
   case 2:ok=run_segment(HEAD_CHANNEL,HEAD_RIGHT_US,AUTO_HEAD_MOVE_MS,g);break;
   default:ok=run_segment(NOD_CHANNEL,NOD_BACKWARD_US,AUTO_NOD_MOVE_MS,g);break;
  }
  step=(step+1)%4;if(ok)(void)wait_cancellable(pause,g);
 }
}
bool servo_head_init(void){
 if(initialized)return true;
 ledc_timer_config_t t={};t.speed_mode=LEDC_LOW_SPEED_MODE;t.duty_resolution=LEDC_TIMER_14_BIT;t.timer_num=LEDC_TIMER_1;t.freq_hz=PWM_FREQ_HZ;t.clk_cfg=LEDC_AUTO_CLK;
 esp_err_t e=ledc_timer_config(&t);if(e!=ESP_OK){ESP_LOGE(TAG,"Timer PWM gagal: %s",esp_err_to_name(e));return false;}
 gpio_num_t pins[]={HEAD_GPIO,NOD_GPIO};ledc_channel_t channels[]={HEAD_CHANNEL,NOD_CHANNEL};
 for(int i=0;i<2;i++){ledc_channel_config_t c={};c.gpio_num=pins[i];c.speed_mode=LEDC_LOW_SPEED_MODE;c.channel=channels[i];c.intr_type=LEDC_INTR_DISABLE;c.timer_sel=LEDC_TIMER_1;c.duty=0;c.hpoint=0;e=ledc_channel_config(&c);if(e!=ESP_OK){ESP_LOGE(TAG,"GPIO%d gagal: %s",int(pins[i]),esp_err_to_name(e));return false;}}
 command_queue=xQueueCreate(4,sizeof(ServoCommand));if(!command_queue)return false;
 if(xTaskCreateWithCaps(servo_worker,"servo_worker",3072,nullptr,5,nullptr,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)!=pdPASS){vQueueDelete(command_queue);command_queue=nullptr;return false;}
 initialized=true;stop_channel(HEAD_CHANNEL);stop_channel(NOD_CHANNEL);
 ESP_LOGI(TAG,"PSRAM servo task stack: 3072 bytes; internal RAM disisakan untuk audio dan driver");
 ESP_LOGI(TAG,"Servo aktif GPIO8/9 PWM 50Hz, FS90R");return true;
}
void servo_head_set_speaking(bool speaking){if(!initialized)return;ai_speaking.store(speaking);if(!speaking){cancel_generation.fetch_add(1);set_gaze_direction(0,0,85);}}
bool servo_head_get_gaze(int *x,int *y,uint32_t *transition_ms,uint32_t *revision,uint32_t *start_tick){
 if(!x||!y||!transition_ms||!revision||!start_tick)return false;
 portENTER_CRITICAL(&gaze_mux);
 *x=gaze_dir_x.load();
 *y=gaze_dir_y.load();
 *transition_ms=gaze_transition_ms.load();
 *start_tick=gaze_start_tick.load();
 *revision=gaze_revision.load();
 portEXIT_CRITICAL(&gaze_mux);
 return true;
}
bool servo_head_command(const char* action){
 if(!initialized||!action)return false;
 if(!strcmp(action,"stop")||!strcmp(action,"servo_stop")){cancel_generation.fetch_add(1);xQueueReset(command_queue);set_gaze_direction(0,0,85);stop_channel(HEAD_CHANNEL);stop_channel(NOD_CHANNEL);return true;}
 ServoAction a;
 if(!strcmp(action,"look_left")||!strcmp(action,"servo_kiri"))a=ServoAction::LookLeft;else if(!strcmp(action,"look_right")||!strcmp(action,"servo_kanan"))a=ServoAction::LookRight;else if(!strcmp(action,"nod")||!strcmp(action,"servo_anguk"))a=ServoAction::Nod;else if(!strcmp(action,"shake")||!strcmp(action,"servo_geleng"))a=ServoAction::Shake;else return false;
 uint32_t g=cancel_generation.fetch_add(1)+1;ServoCommand c{a,g};
 return xQueueSend(command_queue,&c,0)==pdTRUE;
}

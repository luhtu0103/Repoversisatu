#include <cmath>
#include <cstdint>
#include <cstdlib>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/schema/schema_generated.h"
extern const uint8_t model_fp32_tflite_start[] asm("_binary_model_fp32_tflite_start");
extern const uint8_t model_fp32_tflite_end[] asm("_binary_model_fp32_tflite_end");
namespace {
constexpr char kTag[] = "FLAX_TFLM_PROBE";
constexpr size_t kArenaBytes = 4U * 1024U * 1024U;
constexpr int64_t kInvokeTimeoutUs = 120LL * 1000LL * 1000LL;
struct InvokeContext {
 tflite::MicroInterpreter* interpreter;
 TfLiteTensor* output;
 uint8_t* arena;
 TaskHandle_t waiter;
};
void invoke_worker(void* arg) {
 auto* ctx = static_cast<InvokeContext*>(arg);
 const int64_t start_us = esp_timer_get_time();
 const TfLiteStatus status = ctx->interpreter->Invoke();
 const int64_t elapsed_us = esp_timer_get_time() - start_us;
 bool finite = true;
 float l2_squared = 0.0f;
 if (status == kTfLiteOk) {
  for (int i = 0; i < ctx->output->bytes / static_cast<int>(sizeof(float)); ++i) {
   const float value = ctx->output->data.f[i];
   if (!std::isfinite(value)) { finite = false; ESP_LOGE(kTag, "FAIL non-finite output at index=%d", i); break; }
   l2_squared += value * value;
  }
 }
 if (status != kTfLiteOk) {
  ESP_LOGE(kTag, "FAIL Invoke() status=%d latency_ms=%lld", static_cast<int>(status), static_cast<long long>(elapsed_us / 1000));
 } else if (finite) {
  ESP_LOGI(kTag, "ZERO_INPUT_INVOKE_PASS latency_ms=%lld output_l2=%.5f", static_cast<long long>(elapsed_us / 1000), std::sqrt(l2_squared));
  ESP_LOGI(kTag, "heap_after internal_free=%u internal_largest=%u psram_free=%u", static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)), static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)), static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
  ESP_LOGI(kTag, "NOTE runtime loading only; real-audio accuracy/frontend parity are not tested");
 }
 heap_caps_free(ctx->arena);
 xTaskNotifyGive(ctx->waiter);
 vTaskDelete(nullptr);
}
}
extern "C" void app_main(void) {
 const size_t model_bytes = static_cast<size_t>(model_fp32_tflite_end - model_fp32_tflite_start);
 ESP_LOGI(kTag, "START target=ESP32-S3 model=FP32 model_bytes=%u arena_bytes=%u", static_cast<unsigned>(model_bytes), static_cast<unsigned>(kArenaBytes));
 ESP_LOGI(kTag, "heap_before internal_free=%u internal_largest=%u psram_free=%u", static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)), static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)), static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
 const tflite::Model* model = tflite::GetModel(model_fp32_tflite_start);
 if (model == nullptr) { ESP_LOGE(kTag, "FAIL invalid TFLite model pointer"); return; }
 auto* arena = static_cast<uint8_t*>(heap_caps_malloc(kArenaBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
 if (arena == nullptr) { ESP_LOGE(kTag, "FAIL cannot allocate %u-byte tensor arena in PSRAM", static_cast<unsigned>(kArenaBytes)); return; }
 // FP32 baseline avoids the rejected FLOAT32-input/INT8-filter hybrid path.
 tflite::MicroMutableOpResolver<24> resolver;
 if (resolver.AddAdd() != kTfLiteOk || resolver.AddConcatenation() != kTfLiteOk || resolver.AddFullyConnected() != kTfLiteOk || resolver.AddLogistic() != kTfLiteOk || resolver.AddMul() != kTfLiteOk || resolver.AddReshape() != kTfLiteOk || resolver.AddTanh() != kTfLiteOk || resolver.AddSub() != kTfLiteOk || resolver.AddDiv() != kTfLiteOk || resolver.AddExp() != kTfLiteOk || resolver.AddMaximum() != kTfLiteOk || resolver.AddMinimum() != kTfLiteOk || resolver.AddLess() != kTfLiteOk || resolver.AddSlice() != kTfLiteOk || resolver.AddSum() != kTfLiteOk || resolver.AddSqrt() != kTfLiteOk || resolver.AddRsqrt() != kTfLiteOk || resolver.AddReduceMax() != kTfLiteOk || resolver.AddPack() != kTfLiteOk || resolver.AddBroadcastTo() != kTfLiteOk || resolver.AddWhile() != kTfLiteOk || resolver.AddDynamicUpdateSlice() != kTfLiteOk) {
  ESP_LOGE(kTag, "FAIL cannot register one or more probe operators"); heap_caps_free(arena); return;
 }
 tflite::MicroInterpreter interpreter(model, resolver, arena, kArenaBytes);
 const int64_t alloc_start_us = esp_timer_get_time();
 const TfLiteStatus alloc_status = interpreter.AllocateTensors();
 const int64_t alloc_elapsed_us = esp_timer_get_time() - alloc_start_us;
 ESP_LOGI(kTag, "allocate_tensors status=%s elapsed_ms=%lld arena_used=%u", alloc_status == kTfLiteOk ? "OK" : "FAIL", static_cast<long long>(alloc_elapsed_us / 1000), static_cast<unsigned>(interpreter.arena_used_bytes()));
 if (alloc_status != kTfLiteOk) { ESP_LOGE(kTag, "FAIL operator registration, model allocation, or tensor arena"); heap_caps_free(arena); return; }
 TfLiteTensor* input = interpreter.input(0); TfLiteTensor* output = interpreter.output(0);
 if (input == nullptr || output == nullptr || input->type != kTfLiteFloat32 || output->type != kTfLiteFloat32 || input->dims->size != 3 || input->dims->data[0] != 1 || input->dims->data[1] != 180 || input->dims->data[2] != 80 || output->dims->size != 2 || output->dims->data[0] != 1 || output->dims->data[1] != 192) {
  ESP_LOGE(kTag, "FAIL tensor contract mismatch; expected input float32[1,180,80], output float32[1,192]"); heap_caps_free(arena); return;
 }
 ESP_LOGI(kTag, "tensor_contract PASS input=float32[1,180,80] output=float32[1,192]");
 for (int i = 0; i < input->bytes / static_cast<int>(sizeof(float)); ++i) input->data.f[i] = 0.0f;
 InvokeContext ctx{&interpreter, output, arena, xTaskGetCurrentTaskHandle()};
 TaskHandle_t worker = nullptr;
 const BaseType_t task_ok = xTaskCreatePinnedToCore(invoke_worker, "flax_invoke", 16384, &ctx, 5, &worker, 1);
 if (task_ok != pdPASS) { ESP_LOGE(kTag, "FAIL cannot create inference worker on CPU1"); heap_caps_free(arena); return; }
 ESP_LOGI(kTag, "INVOKE_START core=1 timeout_s=120 watchdog-safe diagnostic");
 const int64_t monitor_start_us = esp_timer_get_time();
 while (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000)) == 0) {
  const int64_t elapsed_us = esp_timer_get_time() - monitor_start_us;
  if (elapsed_us >= kInvokeTimeoutUs) {
   ESP_LOGE(kTag, "FAIL Invoke exceeded diagnostic timeout_ms=120000; restarting to avoid an unbounded hang");
   esp_restart();
  }
  ESP_LOGI(kTag, "INVOKE_STILL_RUNNING elapsed_s=%lld", static_cast<long long>(elapsed_us / 1000000));
 }
 ESP_LOGI(kTag, "INVOKE_WORKER_FINISHED");
}

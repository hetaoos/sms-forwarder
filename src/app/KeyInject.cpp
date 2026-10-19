#include "app/KeyInject.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

KeyInject &KeyInject::instance() {
  static KeyInject s;
  return s;
}

KeyInject::KeyInject() {
  _mux = (void *)xSemaphoreCreateMutex();
}

void KeyInject::push(KeyId key, KeyEvent ev) {
  SemaphoreHandle_t mux = (SemaphoreHandle_t)_mux;
  if (mux == nullptr) return;
  if (xSemaphoreTake(mux, pdMS_TO_TICKS(10)) == pdTRUE) {
    if (_q.size() < 16) _q.push({key, ev});
    xSemaphoreGive(mux);
  }
}

bool KeyInject::pop(KeyId &outKey, KeyEvent &outEv) {
  bool got = false;
  SemaphoreHandle_t mux = (SemaphoreHandle_t)_mux;
  if (mux != nullptr && xSemaphoreTake(mux, pdMS_TO_TICKS(10)) == pdTRUE) {
    if (!_q.empty()) {
      const auto &f = _q.front();
      outKey = f.first;
      outEv = f.second;
      _q.pop();
      got = true;
    }
    xSemaphoreGive(mux);
  }
  return got;
}

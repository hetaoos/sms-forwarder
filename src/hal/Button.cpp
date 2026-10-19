#include "hal/Button.h"

#include <Arduino.h>

#include "app/Config.h"

namespace {
// 抖动窗口：电平跳变后需稳定这么久才确认，过滤机械弹跳
constexpr uint16_t kDebounceMs = 25;
}  // namespace

Button::Button(uint8_t pin, bool activeLow, uint16_t longMs, uint16_t repeatMs)
    : _pin(pin), _activeLow(activeLow), _longMs(longMs), _repeatMs(repeatMs) {}

void Button::begin() {
  // 低电平有效用上拉，高电平有效用下拉；纯输入，不改变电平
  pinMode(_pin, _activeLow ? INPUT_PULLUP : INPUT_PULLDOWN);
  _rawState = readPressed();
  _wasPressed = false;
  _debounceUntil = 0;
}

bool Button::readPressed() const {
  int lvl = digitalRead(_pin);
  return _activeLow ? (lvl == LOW) : (lvl == HIGH);
}

KeyEvent Button::poll(uint32_t now) {
  bool pressed = readPressed();

  // 电平跳变：进入抖动窗口，暂不确认
  if (pressed != _rawState) {
    _rawState = pressed;
    _debounceUntil = now + kDebounceMs;
    return KeyEvent::kNone;
  }
  if (now < _debounceUntil) return KeyEvent::kNone;
  bool stable = pressed;

  // 上升沿：按下开始
  if (stable && !_wasPressed) {
    _wasPressed = true;
    _pressStart = now;
    _longFired = false;
    _lastRepeat = now;
    return KeyEvent::kNone;  // 等释放或长按再出事件
  }
  // 按住中：先看是否到长按，再看连发
  if (stable && _wasPressed) {
    if (!_longFired && now - _pressStart >= _longMs) {
      _longFired = true;
      _lastRepeat = now;
      return KeyEvent::kLongPress;
    }
    if (_longFired && now - _lastRepeat >= _repeatMs) {
      _lastRepeat = now;
      return KeyEvent::kRepeat;
    }
    return KeyEvent::kNone;
  }
  // 下降沿：释放
  if (!stable && _wasPressed) {
    _wasPressed = false;
    if (!_longFired) return KeyEvent::kClick;  // 未达长按 -> 短按
    return KeyEvent::kNone;                    // 已发过长按，释放不再报
  }
  return KeyEvent::kNone;
}

Keypad::Keypad()
    : _ok(cfg::kBtnOk, true, cfg::kBtnLongMs, cfg::kBtnRepeatMs),
      _up(cfg::kBtnUp, true, cfg::kBtnLongMs, cfg::kBtnRepeatMs),
      _down(cfg::kBtnDown, true, cfg::kBtnLongMs, cfg::kBtnRepeatMs),
      _back(cfg::kBtnBack, true, cfg::kBtnLongMs, cfg::kBtnRepeatMs),
      _btns{&_ok, &_up, &_down, &_back} {}

void Keypad::begin() {
  for (auto *b : _btns) b->begin();
}

bool Keypad::poll(uint32_t now, KeyId &outKey, KeyEvent &outEvent) {
  for (int i = 0; i < (int)KeyId::kCount; i++) {
    KeyEvent e = _btns[i]->poll(now);
    if (e != KeyEvent::kNone) {
      outKey = (KeyId)i;
      outEvent = e;
      return true;
    }
  }
  return false;
}

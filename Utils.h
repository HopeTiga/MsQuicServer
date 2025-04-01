#pragma once
#include <string>
#include <iostream>
#include <chrono>
#include "MsQuicClient.h"
#include "msquic.hpp"

// 修改函数声明，使用const引用而不是传值
extern void LogEvent(const std::string& eventType, const std::string& details);

extern std::function<void(MsQuicClient*, HQUIC)> func;



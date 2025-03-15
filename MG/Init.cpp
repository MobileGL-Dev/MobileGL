//
// Created by BZLZHH on 2025/3/15.
//

#include "Includes.h"

__attribute__((constructor)) void MG_Initialize() {
    MG_Util::Debug::LogInit();
    MG_Util::Debug::LogI("MobileGL Initializing...");
}
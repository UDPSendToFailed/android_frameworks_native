/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "KeyLayoutMap"

#include <android-base/logging.h>
#include <android/keycodes.h>
#include <ftl/enum.h>
#include <input/InputEventLabels.h>
#include <input/KeyLayoutMap.h>
#include <input/Keyboard.h>
#include <log/log.h>
#include <utils/Errors.h>
#include <utils/Timers.h>
#include <utils/Tokenizer.h>
#if defined(__ANDROID__)
#include <vintf/KernelConfigs.h>
#endif

#include <cstdlib>
#include <string_view>
#include <unordered_map>

/**
 * Log debug output for the parser.
 * Enable this via "adb shell setprop log.tag.KeyLayoutMapParser DEBUG" (requires restart)
 */
const bool DEBUG_PARSER =
        __android_log_is_loggable(ANDROID_LOG_DEBUG, LOG_TAG "Parser", ANDROID_LOG_INFO);

// Enables debug output for parser performance.
#define DEBUG_PARSER_PERFORMANCE 0

/**
 * Log debug output for mapping.
 * Enable this via "adb shell setprop log.tag.KeyLayoutMapMapping DEBUG" (requires restart)
 */
const bool DEBUG_MAPPING =
        __android_log_is_loggable(ANDROID_LOG_DEBUG, LOG_TAG "Mapping", ANDROID_LOG_INFO);

namespace android {
namespace {

std::optional<int> parseInt(const char* str) {
    char* end;
    errno = 0;
    const int value = strtol(str, &end, 0);
    if (end == str) {
        LOG(ERROR) << "Could not parse " << str;
        return {};
    }
    if (errno == ERANGE) {
        LOG(ERROR) << "Out of bounds: " << str;
        return {};
    }
    return value;
}

constexpr const char* WHITESPACE = " \t\r";

template <InputDeviceSensorType S>
constexpr auto sensorPair() {
    return std::make_pair(ftl::enum_name<S>(), S);
}

static const std::unordered_map<std::string_view, InputDeviceSensorType> SENSOR_LIST =
        {sensorPair<InputDeviceSensorType::ACCELEROMETER>(),
         sensorPair<InputDeviceSensorType::MAGNETIC_FIELD>(),
         sensorPair<InputDeviceSensorType::ORIENTATION>(),
         sensorPair<InputDeviceSensorType::GYROSCOPE>(),
         sensorPair<InputDeviceSensorType::LIGHT>(),
         sensorPair<InputDeviceSensorType::PRESSURE>(),
         sensorPair<InputDeviceSensorType::TEMPERATURE>(),
         sensorPair<InputDeviceSensorType::PROXIMITY>(),
         sensorPair<InputDeviceSensorType::GRAVITY>(),
         sensorPair<InputDeviceSensorType::LINEAR_ACCELERATION>(),
         sensorPair<InputDeviceSensorType::ROTATION_VECTOR>(),
         sensorPair<InputDeviceSensorType::RELATIVE_HUMIDITY>(),
         sensorPair<InputDeviceSensorType::AMBIENT_TEMPERATURE>(),
         sensorPair<InputDeviceSensorType::MAGNETIC_FIELD_UNCALIBRATED>(),
         sensorPair<InputDeviceSensorType::GAME_ROTATION_VECTOR>(),
         sensorPair<InputDeviceSensorType::GYROSCOPE_UNCALIBRATED>(),
         sensorPair<InputDeviceSensorType::SIGNIFICANT_MOTION>()};

bool kernelConfigsArePresent(const std::set<std::string>& configs) {
#if defined(__ANDROID__)
    if (configs.empty()) {
        return true;
    }

    std::map<std::string, std::string> kernelConfigs;
    const status_t result = android::kernelconfigs::LoadKernelConfigs(&kernelConfigs);
    LOG_ALWAYS_FATAL_IF(result != OK, "Kernel configs could not be fetched");

    for (const std::string& requiredConfig : configs) {
        const auto configIt = kernelConfigs.find(requiredConfig);
        if (configIt == kernelConfigs.end()) {
            ALOGI("Required kernel config %s is not found", requiredConfig.c_str());
            return false;
        }
        const std::string& option = configIt->second;
        if (option != "y" && option != "m") {
            ALOGI("Required kernel config %s has option %s", requiredConfig.c_str(),
                  option.c_str());
            return false;
        }
    }
    return true;
#else
    (void)configs; // Suppress 'unused variable' warning
    return true;
#endif
}

} // namespace

KeyLayoutMap::KeyLayoutMap() = default;
KeyLayoutMap::~KeyLayoutMap() = default;

base::Result<std::shared_ptr<KeyLayoutMap>> KeyLayoutMap::loadContents(const std::string& filename,
                                                                       const char* contents) {
    return load(filename, contents);
}

base::Result<std::shared_ptr<KeyLayoutMap>> KeyLayoutMap::load(const std::string& filename,
                                                               const char* contents) {
    Tokenizer* tokenizer;
    status_t status;
    if (contents == nullptr) {
        status = Tokenizer::open(String8(filename.c_str()), &tokenizer);
    } else {
        status = Tokenizer::fromContents(String8(filename.c_str()), contents, &tokenizer);
    }
    if (status) {
        ALOGE("Error %d opening key layout map file %s.", status, filename.c_str());
        return Errorf("Error {} opening key layout map file {}.", status, filename.c_str());
    }
    std::unique_ptr<Tokenizer> t(tokenizer);
    auto ret = load(t.get());
    if (!ret.ok()) {
        return ret;
    }
    const std::shared_ptr<KeyLayoutMap>& map = *ret;
    LOG_ALWAYS_FATAL_IF(map == nullptr, "Returned map should not be null if there's no error");
    if (!kernelConfigsArePresent(map->mRequiredKernelConfigs)) {
        ALOGI("Not loading %s because the required kernel configs are not set", filename.c_str());
        return Errorf("Missing kernel config");
    }
    map->mLoadFileName = filename;
    return ret;
}

base::Result<std::shared_ptr<KeyLayoutMap>> KeyLayoutMap::load(Tokenizer* tokenizer) {
    std::shared_ptr<KeyLayoutMap> map = std::shared_ptr<KeyLayoutMap>(new KeyLayoutMap());
    status_t status = OK;
    if (!map.get()) {
        ALOGE("Error allocating key layout map.");
        return Errorf("Error allocating key layout map.");
    } else {
#if DEBUG_PARSER_PERFORMANCE
        nsecs_t startTime = systemTime(SYSTEM_TIME_MONOTONIC);
#endif
        Parser parser(map.get(), tokenizer);
        status = parser.parse();
#if DEBUG_PARSER_PERFORMANCE
        nsecs_t elapsedTime = systemTime(SYSTEM_TIME_MONOTONIC) - startTime;
        ALOGD("Parsed key layout map file '%s' %d lines in %0.3fms.",
              tokenizer->getFilename().c_str(), tokenizer->getLineNumber(),
              elapsedTime / 1000000.0);
#endif
        if (!status) {
            return std::move(map);
        }
    }
    return Errorf("Load KeyLayoutMap failed {}.", status);
}

status_t KeyLayoutMap::mapKey(int32_t scanCode, int32_t usageCode,
        int32_t* outKeyCode, uint32_t* outFlags) const {
    const Key* key = getKey(scanCode, usageCode);
    if (!key) {
        ALOGD_IF(DEBUG_MAPPING, "mapKey: scanCode=%d, usageCode=0x%08x ~ Failed.", scanCode,
                 usageCode);
        *outKeyCode = AKEYCODE_UNKNOWN;
        *outFlags = 0;
        return NAME_NOT_FOUND;
    }

    *outKeyCode = key->keyCode;
    *outFlags = key->flags;

    ALOGD_IF(DEBUG_MAPPING,
             "mapKey: scanCode=%d, usageCode=0x%08x ~ Result keyCode=%d, outFlags=0x%08x.",
             scanCode, usageCode, *outKeyCode, *outFlags);
    return NO_ERROR;
}

// Return pair of sensor type and sensor data index, for the input device abs code
base::Result<std::pair<InputDeviceSensorType, int32_t>> KeyLayoutMap::mapSensor(
        int32_t absCode) const {
    auto it = mSensorsByAbsCode.find(absCode);
    if (it == mSensorsByAbsCode.end()) {
        ALOGD_IF(DEBUG_MAPPING, "mapSensor: absCode=%d, ~ Failed.", absCode);
        return Errorf("Can't find abs code {}.", absCode);
    }
    const Sensor& sensor = it->second;
    ALOGD_IF(DEBUG_MAPPING, "mapSensor: absCode=%d, sensorType=%s, sensorDataIndex=0x%x.", absCode,
             ftl::enum_string(sensor.sensorType).c_str(), sensor.sensorDataIndex);
    return std::make_pair(sensor.sensorType, sensor.sensorDataIndex);
}

const KeyLayoutMap::Key* KeyLayoutMap::getKey(int32_t scanCode, int32_t usageCode) const {
    if (usageCode) {
        auto it = mKeysByUsageCode.find(usageCode);
        if (it != mKeysByUsageCode.end()) {
            return &it->second;
        }
    }
    if (scanCode) {
        auto it = mKeysByScanCode.find(scanCode);
        if (it != mKeysByScanCode.end()) {
            return &it->second;
        }
    }
    return nullptr;
}

std::vector<int32_t> KeyLayoutMap::findScanCodesForKey(int32_t keyCode) const {
    std::vector<int32_t> scanCodes;
    // b/354333072: Only consider keys without FUNCTION flag
    for (const auto& [scanCode, key] : mKeysByScanCode) {
        if (keyCode == key.keyCode && !(key.flags & POLICY_FLAG_FUNCTION)) {
            scanCodes.push_back(scanCode);
        }
    }
    return scanCodes;
}

std::vector<int32_t> KeyLayoutMap::findUsageCodesForKey(int32_t keyCode) const {
    std::vector<int32_t> usageCodes;
    for (const auto& [usageCode, key] : mKeysByUsageCode) {
        if (keyCode == key.keyCode && !(key.flags & POLICY_FLAG_FALLBACK_USAGE_MAPPING)) {
            usageCodes.push_back(usageCode);
        }
    }
    return usageCodes;
}

std::optional<AxisInfo> KeyLayoutMap::mapAxis(int32_t scanCode) const {
    auto it = mAxes.find(scanCode);
    if (it == mAxes.end()) {
        ALOGD_IF(DEBUG_MAPPING, "mapAxis: scanCode=%d ~ Failed.", scanCode);
        return std::nullopt;
    }

    const AxisInfo& axisInfo = it->second;
    ALOGD_IF(DEBUG_MAPPING,
             "mapAxis: scanCode=%d ~ Result mode=%d, axis=%d, highAxis=%d, "
             "splitValue=%d, flatOverride=%d.",
             scanCode, axisInfo.mode, axisInfo.axis, axisInfo.highAxis, axisInfo.splitValue,
             axisInfo.flatOverride);
    return axisInfo;
}

std::optional<int32_t> KeyLayoutMap::findScanCodeForLed(int32_t ledCode) const {
    for (const auto& [scanCode, led] : mLedsByScanCode) {
        if (led.ledCode == ledCode) {
            ALOGD_IF(DEBUG_MAPPING, "%s: ledCode=%d, scanCode=%d.", __func__, ledCode, scanCode);
            return scanCode;
        }
    }
    ALOGD_IF(DEBUG_MAPPING, "%s: ledCode=%d ~ Not found.", __func__, ledCode);
    return std::nullopt;
}

std::optional<int32_t> KeyLayoutMap::findUsageCodeForLed(int32_t ledCode) const {
    for (const auto& [usageCode, led] : mLedsByUsageCode) {
        if (led.ledCode == ledCode) {
            ALOGD_IF(DEBUG_MAPPING, "%s: ledCode=%d, usage=%x.", __func__, ledCode, usageCode);
            return usageCode;
        }
    }
    ALOGD_IF(DEBUG_MAPPING, "%s: ledCode=%d ~ Not found.", __func__, ledCode);
    return std::nullopt;
}

// --- KeyLayoutMap::Parser ---

KeyLayoutMap::Parser::Parser(KeyLayoutMap* map, Tokenizer* tokenizer) :
        mMap(map), mTokenizer(tokenizer) {
}

KeyLayoutMap::Parser::~Parser() {
}

status_t KeyLayoutMap::Parser::parse() {
    while (!mTokenizer->isEof()) {
        ALOGD_IF(DEBUG_PARSER, "Parsing %s: '%s'.", mTokenizer->getLocation().c_str(),
                 mTokenizer->peekRemainderOfLine().c_str());

        mTokenizer->skipDelimiters(WHITESPACE);

        if (!mTokenizer->isEol() && mTokenizer->peekChar() != '#') {
            String8 keywordToken = mTokenizer->nextToken(WHITESPACE);
            if (keywordToken == "key") {
                mTokenizer->skipDelimiters(WHITESPACE);
                status_t status = parseKey();
                if (status) return status;
            } else if (keywordToken == "axis") {
                mTokenizer->skipDelimiters(WHITESPACE);
                status_t status = parseAxis();
                if (status) return status;
            } else if (keywordToken == "led") {
                mTokenizer->skipDelimiters(WHITESPACE);
                status_t status = parseLed();
                if (status) return status;
            } else if (keywordToken == "sensor") {
                mTokenizer->skipDelimiters(WHITESPACE);
                status_t status = parseSensor();
                if (status) return status;
            } else if (keywordToken == "requires_kernel_config") {
                mTokenizer->skipDelimiters(WHITESPACE);
                status_t status = parseRequiredKernelConfig();
                if (status) return status;
            } else {
                ALOGE("%s: Expected keyword, got '%s'.", mTokenizer->getLocation().c_str(),
                      keywordToken.c_str());
                return BAD_VALUE;
            }

            mTokenizer->skipDelimiters(WHITESPACE);
            if (!mTokenizer->isEol() && mTokenizer->peekChar() != '#') {
                ALOGE("%s: Expected end of line or trailing comment, got '%s'.",
                      mTokenizer->getLocation().c_str(), mTokenizer->peekRemainderOfLine().c_str());
                return BAD_VALUE;
            }
        }

        mTokenizer->nextLine();
    }
    return NO_ERROR;
}

status_t KeyLayoutMap::Parser::parseKey() {
    String8 codeToken = mTokenizer->nextToken(WHITESPACE);
    bool mapUsage = false;
    if (codeToken == "usage") {
        mapUsage = true;
        mTokenizer->skipDelimiters(WHITESPACE);
        codeToken = mTokenizer->nextToken(WHITESPACE);
    }

    std::optional<int> code = parseInt(codeToken.c_str());
    if (!code) {
        ALOGE("%s: Expected key %s number, got '%s'.", mTokenizer->getLocation().c_str(),
                mapUsage ? "usage" : "scan code", codeToken.c_str());
        return BAD_VALUE;
    }
    std::unordered_map<int32_t, Key>& map =
            mapUsage ? mMap->mKeysByUsageCode : mMap->mKeysByScanCode;
    if (map.find(*code) != map.end()) {
        ALOGE("%s: Duplicate entry for key %s '%s'.", mTokenizer->getLocation().c_str(),
                mapUsage ? "usage" : "scan code", codeToken.c_str());
        return BAD_VALUE;
    }

    mTokenizer->skipDelimiters(WHITESPACE);
    String8 keyCodeToken = mTokenizer->nextToken(WHITESPACE);
    std::optional<int> keyCode = InputEventLookup::getKeyCodeByLabel(keyCodeToken.c_str());
    if (!keyCode) {
        ALOGE("%s: Expected key code label, got '%s'.", mTokenizer->getLocation().c_str(),
              keyCodeToken.c_str());
        return BAD_VALUE;
    }

    uint32_t flags = 0;
    for (;;) {
        mTokenizer->skipDelimiters(WHITESPACE);
        if (mTokenizer->isEol() || mTokenizer->peekChar() == '#') break;

        String8 flagToken = mTokenizer->nextToken(WHITESPACE);
        std::optional<int> flag = InputEventLookup::getKeyFlagByLabel(flagToken.c_str());
        if (!flag) {
            ALOGE("%s: Expected key flag label, got '%s'.", mTokenizer->getLocation().c_str(),
                  flagToken.c_str());
            return BAD_VALUE;
        }
        if (flags & *flag) {
            ALOGE("%s: Duplicate key flag '%s'.", mTokenizer->getLocation().c_str(),
                    flagToken.c_str());
            return BAD_VALUE;
        }
        flags |= *flag;
    }

    ALOGD_IF(DEBUG_PARSER, "Parsed key %s: code=%d, keyCode=%d, flags=0x%08x.",
             mapUsage ? "usage" : "scan code", *code, *keyCode, flags);

    Key key;
    key.keyCode = *keyCode;
    key.flags = flags;
    map.insert({*code, key});
    return NO_ERROR;
}

status_t KeyLayoutMap::Parser::parseAxis() {
    String8 scanCodeToken = mTokenizer->nextToken(WHITESPACE);
    std::optional<int> scanCode = parseInt(scanCodeToken.c_str());
    if (!scanCode) {
        ALOGE("%s: Expected axis scan code number, got '%s'.", mTokenizer->getLocation().c_str(),
                scanCodeToken.c_str());
        return BAD_VALUE;
    }
    if (mMap->mAxes.find(*scanCode) != mMap->mAxes.end()) {
        ALOGE("%s: Duplicate entry for axis scan code '%s'.", mTokenizer->getLocation().c_str(),
                scanCodeToken.c_str());
        return BAD_VALUE;
    }

    AxisInfo axisInfo;

    mTokenizer->skipDelimiters(WHITESPACE);
    String8 token = mTokenizer->nextToken(WHITESPACE);
    if (token == "invert") {
        axisInfo.mode = AxisInfo::MODE_INVERT;

        mTokenizer->skipDelimiters(WHITESPACE);
        String8 axisToken = mTokenizer->nextToken(WHITESPACE);
        std::optional<int> axis = InputEventLookup::getAxisByLabel(axisToken.c_str());
        if (!axis) {
            ALOGE("%s: Expected inverted axis label, got '%s'.",
                    mTokenizer->getLocation().c_str(), axisToken.c_str());
            return BAD_VALUE;
        }
        axisInfo.axis = *axis;
    } else if (token == "split") {
        axisInfo.mode = AxisInfo::MODE_SPLIT;

        mTokenizer->skipDelimiters(WHITESPACE);
        String8 splitToken = mTokenizer->nextToken(WHITESPACE);
        std::optional<int> splitValue = parseInt(splitToken.c_str());
        if (!splitValue) {
            ALOGE("%s: Expected split value, got '%s'.",
                    mTokenizer->getLocation().c_str(), splitToken.c_str());
            return BAD_VALUE;
        }
        axisInfo.splitValue = *splitValue;

        mTokenizer->skipDelimiters(WHITESPACE);
        String8 lowAxisToken = mTokenizer->nextToken(WHITESPACE);
        std::optional<int> axis = InputEventLookup::getAxisByLabel(lowAxisToken.c_str());
        if (!axis) {
            ALOGE("%s: Expected low axis label, got '%s'.",
                    mTokenizer->getLocation().c_str(), lowAxisToken.c_str());
            return BAD_VALUE;
        }
        axisInfo.axis = *axis;

        mTokenizer->skipDelimiters(WHITESPACE);
        String8 highAxisToken = mTokenizer->nextToken(WHITESPACE);
        std::optional<int> highAxis = InputEventLookup::getAxisByLabel(highAxisToken.c_str());
        if (!highAxis) {
            ALOGE("%s: Expected high axis label, got '%s'.",
                    mTokenizer->getLocation().c_str(), highAxisToken.c_str());
            return BAD_VALUE;
        }
        axisInfo.highAxis = *highAxis;
    } else {
        std::optional<int> axis = InputEventLookup::getAxisByLabel(token.c_str());
        if (!axis) {
            ALOGE("%s: Expected axis label, 'split' or 'invert', got '%s'.",
                  mTokenizer->getLocation().c_str(), token.c_str());
            return BAD_VALUE;
        }
        axisInfo.axis = *axis;
    }

    for (;;) {
        mTokenizer->skipDelimiters(WHITESPACE);
        if (mTokenizer->isEol() || mTokenizer->peekChar() == '#') {
            break;
        }
        String8 keywordToken = mTokenizer->nextToken(WHITESPACE);
        if (keywordToken == "flat") {
            mTokenizer->skipDelimiters(WHITESPACE);
            String8 flatToken = mTokenizer->nextToken(WHITESPACE);
            std::optional<int> flatOverride = parseInt(flatToken.c_str());
            if (!flatOverride) {
                ALOGE("%s: Expected flat value, got '%s'.",
                        mTokenizer->getLocation().c_str(), flatToken.c_str());
                return BAD_VALUE;
            }
            axisInfo.flatOverride = *flatOverride;
        } else {
            ALOGE("%s: Expected keyword 'flat', got '%s'.", mTokenizer->getLocation().c_str(),
                  keywordToken.c_str());
            return BAD_VALUE;
        }
    }

    ALOGD_IF(DEBUG_PARSER,
             "Parsed axis: scanCode=%d, mode=%d, axis=%d, highAxis=%d, "
             "splitValue=%d, flatOverride=%d.",
             *scanCode, axisInfo.mode, axisInfo.axis, axisInfo.highAxis, axisInfo.splitValue,
             axisInfo.flatOverride);
    mMap->mAxes.insert({*scanCode, axisInfo});
    return NO_ERROR;
}

status_t KeyLayoutMap::Parser::parseLed() {
    String8 codeToken = mTokenizer->nextToken(WHITESPACE);
    bool mapUsage = false;
    if (codeToken == "usage") {
        mapUsage = true;
        mTokenizer->skipDelimiters(WHITESPACE);
        codeToken = mTokenizer->nextToken(WHITESPACE);
    }
    std::optional<int> code = parseInt(codeToken.c_str());
    if (!code) {
        ALOGE("%s: Expected led %s number, got '%s'.", mTokenizer->getLocation().c_str(),
                mapUsage ? "usage" : "scan code", codeToken.c_str());
        return BAD_VALUE;
    }

    std::unordered_map<int32_t, Led>& map =
            mapUsage ? mMap->mLedsByUsageCode : mMap->mLedsByScanCode;
    if (map.find(*code) != map.end()) {
        ALOGE("%s: Duplicate entry for led %s '%s'.", mTokenizer->getLocation().c_str(),
                mapUsage ? "usage" : "scan code", codeToken.c_str());
        return BAD_VALUE;
    }

    mTokenizer->skipDelimiters(WHITESPACE);
    String8 ledCodeToken = mTokenizer->nextToken(WHITESPACE);
    std::optional<int> ledCode = InputEventLookup::getLedByLabel(ledCodeToken.c_str());
    if (!ledCode) {
        ALOGE("%s: Expected LED code label, got '%s'.", mTokenizer->getLocation().c_str(),
                ledCodeToken.c_str());
        return BAD_VALUE;
    }

    ALOGD_IF(DEBUG_PARSER, "Parsed led %s: code=%d, ledCode=%d.", mapUsage ? "usage" : "scan code",
             *code, *ledCode);

    Led led;
    led.ledCode = *ledCode;
    map.insert({*code, led});
    return NO_ERROR;
}

static std::optional<InputDeviceSensorType> getSensorType(const char* token) {
    auto it = SENSOR_LIST.find(token);
    if (it == SENSOR_LIST.end()) {
        return std::nullopt;
    }
    return it->second;
}

static std::optional<int32_t> getSensorDataIndex(String8 token) {
    std::string tokenStr(token.c_str());
    if (tokenStr == "X") {
        return 0;
    } else if (tokenStr == "Y") {
        return 1;
    } else if (tokenStr == "Z") {
        return 2;
    }
    return std::nullopt;
}

// Parse sensor type and data index mapping, as below format
// sensor <raw abs> <sensor type> <sensor data index>
// raw abs : the linux abs code of the axis
// sensor type : string name of InputDeviceSensorType
// sensor data index : the data index of sensor, out of [X, Y, Z]
// Examples:
// sensor 0x00 ACCELEROMETER X
// sensor 0x01 ACCELEROMETER Y
// sensor 0x02 ACCELEROMETER Z
// sensor 0x03 GYROSCOPE X
// sensor 0x04 GYROSCOPE Y
// sensor 0x05 GYROSCOPE Z
status_t KeyLayoutMap::Parser::parseSensor() {
    String8 codeToken = mTokenizer->nextToken(WHITESPACE);
    std::optional<int> code = parseInt(codeToken.c_str());
    if (!code) {
        ALOGE("%s: Expected sensor %s number, got '%s'.", mTokenizer->getLocation().c_str(),
              "abs code", codeToken.c_str());
        return BAD_VALUE;
    }

    std::unordered_map<int32_t, Sensor>& map = mMap->mSensorsByAbsCode;
    if (map.find(*code) != map.end()) {
        ALOGE("%s: Duplicate entry for sensor %s '%s'.", mTokenizer->getLocation().c_str(),
              "abs code", codeToken.c_str());
        return BAD_VALUE;
    }

    mTokenizer->skipDelimiters(WHITESPACE);
    String8 sensorTypeToken = mTokenizer->nextToken(WHITESPACE);
    std::optional<InputDeviceSensorType> typeOpt = getSensorType(sensorTypeToken.c_str());
    if (!typeOpt) {
        ALOGE("%s: Expected sensor code label, got '%s'.", mTokenizer->getLocation().c_str(),
              sensorTypeToken.c_str());
        return BAD_VALUE;
    }
    InputDeviceSensorType sensorType = typeOpt.value();
    mTokenizer->skipDelimiters(WHITESPACE);
    String8 sensorDataIndexToken = mTokenizer->nextToken(WHITESPACE);
    std::optional<int32_t> indexOpt = getSensorDataIndex(sensorDataIndexToken);
    if (!indexOpt) {
        ALOGE("%s: Expected sensor data index label, got '%s'.", mTokenizer->getLocation().c_str(),
              sensorDataIndexToken.c_str());
        return BAD_VALUE;
    }
    int32_t sensorDataIndex = indexOpt.value();

    ALOGD_IF(DEBUG_PARSER, "Parsed sensor: abs code=%d, sensorType=%s, sensorDataIndex=%d.", *code,
             ftl::enum_string(sensorType).c_str(), sensorDataIndex);

    Sensor sensor;
    sensor.sensorType = sensorType;
    sensor.sensorDataIndex = sensorDataIndex;
    map.emplace(*code, sensor);
    return NO_ERROR;
}

// Parse the name of a required kernel config.
// The layout won't be used if the specified kernel config is not present
// Examples:
// requires_kernel_config CONFIG_HID_PLAYSTATION
status_t KeyLayoutMap::Parser::parseRequiredKernelConfig() {
    String8 codeToken = mTokenizer->nextToken(WHITESPACE);
    std::string configName = codeToken.c_str();

    const auto result = mMap->mRequiredKernelConfigs.emplace(configName);
    if (!result.second) {
        ALOGE("%s: Duplicate entry for required kernel config %s.",
              mTokenizer->getLocation().c_str(), configName.c_str());
        return BAD_VALUE;
    }

    ALOGD_IF(DEBUG_PARSER, "Parsed required kernel config: name=%s", configName.c_str());
    return NO_ERROR;
}

} // namespace android

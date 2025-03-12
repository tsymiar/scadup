//
// Created by tsymiar on 2022/6/8.
//

#include "FileUtils.h"
#include <mutex>
#include <fstream>

#define LOG_TAG "FileUtils"
#include "logging.h"
#include <cstdio>
#include <iostream>

static std::once_flag g_create_flag;
static std::shared_ptr<FileUtils> g_instance;

FileUtils& FileUtils::operator=(const FileUtils&) = default;

std::shared_ptr<FileUtils> FileUtils::instance()
{
    std::call_once(
        g_create_flag,
        [&]() {
            struct make_shared_enabler : FileUtils { };
            g_instance = std::make_shared<make_shared_enabler>();
        }
    );
    return g_instance;
}

std::string FileUtils::GetFileStringContent(const std::string& filename)
{
    std::string content{};
    std::ifstream file(filename, std::ios::binary);
    if (!file.fail()) {
        file.seekg(0, std::ios::end);
        content.resize(file.tellg());
        file.seekg(0, std::ios::beg);
        size_t size = content.size();
        if (size > 0x1000000) {
            size = 0x1000000;
        }
        file.read(&content[0], size);
        file.close();
    }
    return content;
}

std::string FileUtils::getStrFile2string(const std::string& filename)
{
    std::string content{};
    std::ifstream file(filename);
    if (file.is_open()) {
        content.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
    file.close();
    return content;
}

std::string FileUtils::getVariable(const std::string& url, const std::string& key)
{
    std::string val = {};
    size_t pos = url.find(key);
    if (pos != std::string::npos) {
        val = url.substr(pos, url.size());
        pos = val.find('=');
        size_t org = val.find('&');
        if (org == std::string::npos) {
            val = val.substr(pos + 1, val.size() - pos - 1);
        } else {
            val = val.substr(pos + 1, org - pos - 1);
        }
    }
    if ((pos = val.find('\n')) != std::string::npos) {
        val.resize(pos);
    }
    return val;
}

//
// Created by tsymiar on 2022/6/8.
//

#ifndef FILEUTILS_H
#define FILEUTILS_H

#include <memory>
#include <string>

class FileUtils {
public:
    static std::shared_ptr <FileUtils> instance();

    static std::string GetFileStringContent(const std::string& filename);

    static std::string getStrFile2string(const std::string& filename);

    static std::string getVariable(const std::string& url, const std::string& key);

private:
    FileUtils() = default;

    ~FileUtils() = default;

    FileUtils(const FileUtils&) = default;

    FileUtils& operator=(const FileUtils&);
};

#endif //DEVICE2DEVICE_FILEUTILS_H
